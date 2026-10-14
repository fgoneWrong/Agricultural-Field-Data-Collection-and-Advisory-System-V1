#include "local_water_sensor.h"

#include <Arduino.h>
#include <HCSR04.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace LocalWaterSensor {
namespace {

// Printed ESP32-S3 GPIO numbers, not physical header positions. Sensor VCC is
// 3.3V; Mode is left open for the factory-default trigger/echo (Mode 0) protocol.
constexpr uint8_t TRIGGER_PIN = 5;
constexpr uint8_t ECHO_PIN = 6;
constexpr uint16_t MAX_DISTANCE_CM = 500;
constexpr unsigned long ECHO_TIMEOUT_US = 40000;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t FRESH_SAMPLE_MS = 3500;
constexpr float MIN_DISTANCE_MM = 230.0f;
constexpr float MAX_DISTANCE_MM = 5000.0f;
constexpr size_t MAX_REFERENCE_TEXT_LENGTH = 16;
constexpr size_t MAX_REFERENCE_REQUEST_BYTES = 96;

// V3.3 datasheet: 23..500cm, >=10us trigger, >50ms measurement period,
// and no-echo ECHO termination after 40ms. The HCSR04 driver additionally caps
// its pulse wait at the timeout implied by its 500cm range (about 36.5ms).
// https://make.net.za/wp-content/datasheets/JSN-SR04T-V3.3%20Datasheet%20-%20Original.pdf
// https://github.com/Martinsos/arduino-lib-hc-sr04

struct WaterSnapshot {
  float distanceMm = NAN;
  // Probe-face height above the chosen zero-water plane. No guessed baseline;
  // this is RAM-only and deliberately returns to unset after every restart.
  float referenceHeightMm = NAN;
  uint32_t capturedAt = 0;
  uint32_t sequence = 0;
  bool hasSample = false;
  bool latestReadingValid = false;
  const char* status = "Waiting for the JSN-SR04T-V3.3.";
};

WaterSnapshot sharedSnapshot;
SemaphoreHandle_t snapshotMutex = nullptr;
bool started = false;

bool validDistanceMm(float distanceMm) {
  return std::isfinite(distanceMm) && distanceMm >= MIN_DISTANCE_MM &&
         distanceMm <= MAX_DISTANCE_MM;
}

float waterLevelMm(float distanceMm, float referenceHeightMm) {
  if (!validDistanceMm(distanceMm) || !validDistanceMm(referenceHeightMm) ||
      distanceMm > referenceHeightMm) {
    return NAN;  // A negative depth signals an invalid reference, not dry land.
  }
  return referenceHeightMm - distanceMm;
}

bool parseReferenceHeight(const char* text, size_t length, float& result) {
  if (text == nullptr || length == 0 || length > MAX_REFERENCE_TEXT_LENGTH) {
    return false;
  }

  // Strict unsigned decimal: reject signs, exponents, whitespace, embedded NUL
  // and incomplete decimals. Check bounds in double precision before rounding.
  bool decimalPointSeen = false;
  for (size_t i = 0; i < length; ++i) {
    const char ch = text[i];
    if (ch >= '0' && ch <= '9') continue;
    if (ch == '.' && !decimalPointSeen && i > 0 && i + 1 < length) {
      decimalPointSeen = true;
      continue;
    }
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(text, &end);
  if (errno == ERANGE || end != text + length || !std::isfinite(value) ||
      value < MIN_DISTANCE_MM || value > MAX_DISTANCE_MM) {
    return false;
  }
  result = static_cast<float>(value);
  return true;
}

WaterSnapshot readSnapshot() {
  WaterSnapshot result;
  if (snapshotMutex == nullptr) {
    result.status = "Water sensor unavailable: synchronization not initialized.";
    return result;
  }
  if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    result.status = "Water sensor unavailable: snapshot busy.";
    return result;
  }
  result = sharedSnapshot;
  xSemaphoreGive(snapshotMutex);
  return result;
}

void publishReading(float distanceCm, uint32_t capturedAt) {
  const float distanceMm = distanceCm * 10.0f;
  const char* failure = nullptr;
  if (!std::isfinite(distanceCm) || distanceCm <= 0.0f) {
    failure = "JSN-SR04T has no valid echo; check target, Mode 0 and wiring with power off.";
  } else if (distanceMm < MIN_DISTANCE_MM) {
    failure = "Target is inside the JSN-SR04T 230 mm blind zone; reading unavailable.";
  } else if (!validDistanceMm(distanceMm)) {
    failure = "JSN-SR04T distance is outside its 230 to 5000 mm range.";
  }

  // No hardware calls while holding this mutex. A reference update cannot be
  // overwritten by the sampler: it changes only the measurement fields here.
  if (xSemaphoreTake(snapshotMutex, portMAX_DELAY) != pdTRUE) return;
  sharedSnapshot.latestReadingValid = failure == nullptr;
  if (failure != nullptr) {
    sharedSnapshot.status = failure;
    // Keep last-success age/sequence for diagnosis, but never expose that old
    // distance as current after a failed acquisition.
  } else {
    sharedSnapshot.distanceMm = distanceMm;
    sharedSnapshot.capturedAt = capturedAt;
    ++sharedSnapshot.sequence;
    sharedSnapshot.hasSample = true;
    sharedSnapshot.status = "Live JSN-SR04T distance.";
  }
  xSemaphoreGive(snapshotMutex);
}

void samplingTask(void*) {
  // The driver constructor calls pinMode, so it must not run before setup or
  // from any HTTP handler. This task is the sole owner of both sensor pins.
  UltraSonicDistanceSensor waterSensor(TRIGGER_PIN, ECHO_PIN, MAX_DISTANCE_CM,
                                     ECHO_TIMEOUT_US);
  // Allow the sensor to settle after power-up without delaying web startup.
  vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
  for (;;) {
    const float distanceCm = waterSensor.measureDistanceCm();
    publishReading(distanceCm, millis());
    // Delay after each bounded measurement, avoiding burst catch-up triggers
    // if this low-priority task was delayed. No extra temperature correction.
    vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
  }
}

void sendSnapshot(WebServer& server) {
  JsonDocument doc;
  doc["ok"] = true;  // HTTP succeeded; water_sensor_ok describes the sensor.
  appendSnapshot(doc.as<JsonObject>());
  doc["uptime_ms"] = millis();
  String body;
  body.reserve(768);
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(200, "application/json; charset=utf-8", body);
}

void sendReferenceError(WebServer& server, int statusCode, const char* message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;
  String body;
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(statusCode, "application/json; charset=utf-8", body);
}

void handleReferenceUpdate(WebServer& server) {
  // Same-origin browser guard, not authentication. No CORS permission is given.
  if (server.header("X-EFB-Request") != "1") {
    sendReferenceError(server, 403, "Use the water reference control on this device.");
    return;
  }
  String contentType = server.header("Content-Type");
  const int separator = contentType.indexOf(';');
  if (separator >= 0) contentType = contentType.substring(0, separator);
  contentType.trim();
  if (!contentType.equalsIgnoreCase("application/x-www-form-urlencoded")) {
    sendReferenceError(server, 415, "Send a form-encoded water reference.");
    return;
  }

  // WebServer parsed the body before invoking this handler; these are endpoint
  // acceptance limits, not a claim to limit the WebServer parser's allocation.
  const String lengthText = server.header("Content-Length");
  size_t requestBytes = 0;
  if (lengthText.length() == 0 || lengthText.length() > 3) {
    sendReferenceError(server, 400, "Invalid water reference request size.");
    return;
  }
  for (size_t i = 0; i < lengthText.length(); ++i) {
    const char ch = lengthText[i];
    if (ch < '0' || ch > '9') {
      sendReferenceError(server, 400, "Invalid water reference request size.");
      return;
    }
    requestBytes = requestBytes * 10 + static_cast<size_t>(ch - '0');
  }
  if (requestBytes == 0 || requestBytes > MAX_REFERENCE_REQUEST_BYTES) {
    sendReferenceError(server, 413, "Water reference request must be 1 to 96 bytes.");
    return;
  }
  if (server.args() != 1 || server.argName(0) != "reference_height_mm") {
    sendReferenceError(server, 400, "Submit only reference_height_mm.");
    return;
  }
  const String text = server.arg("reference_height_mm");
  float referenceHeightMm = NAN;
  if (!parseReferenceHeight(text.c_str(), text.length(), referenceHeightMm)) {
    sendReferenceError(server, 400, "Reference must be a decimal from 230 to 5000 mm.");
    return;
  }
  if (snapshotMutex == nullptr ||
      xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    sendReferenceError(server, 503, "Water reference is temporarily unavailable; retry.");
    return;
  }
  sharedSnapshot.referenceHeightMm = referenceHeightMm;
  xSemaphoreGive(snapshotMutex);
  // Even when the sensor is offline, store the reference without triggering a
  // measurement or reviving its previous reading. This never controls a pump.
  sendSnapshot(server);
}

}  // namespace

void begin() {
  if (started) return;
  started = true;
  snapshotMutex = xSemaphoreCreateMutex();
  if (snapshotMutex == nullptr) {
    Serial.println("Local water unavailable: could not create snapshot mutex.");
    return;
  }
  if (xTaskCreatePinnedToCore(samplingTask, "efb-jsn-sr04t", 3072, nullptr, 1,
                            nullptr, 1) != pdPASS) {
    if (xSemaphoreTake(snapshotMutex, portMAX_DELAY) == pdTRUE) {
      sharedSnapshot.status = "Local water unavailable: could not start sampling task.";
      xSemaphoreGive(snapshotMutex);
    }
    Serial.println("Local water unavailable: could not start sampling task.");
    return;
  }
  Serial.println("JSN-SR04T-V3.3 task started: TRIG=5, ECHO=6, Mode 0, 3.3V.");
}

void appendSnapshot(JsonObject doc) {
  const WaterSnapshot snapshot = readSnapshot();
  // Copy before reading time; unsigned subtraction handles millis() rollover.
  const uint32_t now = millis();
  const uint32_t sampleAge = now - snapshot.capturedAt;
  const bool fresh = snapshot.latestReadingValid && snapshot.hasSample &&
                     sampleAge <= FRESH_SAMPLE_MS;
  const float levelMm = fresh
                           ? waterLevelMm(snapshot.distanceMm, snapshot.referenceHeightMm)
                           : NAN;
  const bool levelValid = std::isfinite(levelMm);
  const char* status = snapshot.status;
  if (snapshot.latestReadingValid && snapshot.hasSample && !fresh) {
    status = "Water sensor data is stale; waiting for a new sample.";
  } else if (fresh && !std::isfinite(snapshot.referenceHeightMm)) {
    status = "Live distance only; set probe height above the zero-water reference plane.";
  } else if (fresh && !levelValid) {
    status = "Distance exceeds reference height; check the reference plane and target.";
  } else if (levelValid) {
    status = "Live water level relative to the configured reference plane.";
  }

  doc["water_sensor_model"] = "JSN-SR04T-V3.3";
  doc["water_sensor_ok"] = fresh;
  doc["water_sensor_status"] = status;
  doc["water_sequence"] = snapshot.sequence;
  doc["water_sample_age_ms"] = nullptr;
  doc["water_distance_mm"] = nullptr;
  doc["water_reference_height_mm"] = nullptr;
  doc["water_level_mm"] = nullptr;
  doc["water_level_valid"] = levelValid;
  if (snapshot.hasSample) doc["water_sample_age_ms"] = sampleAge;
  if (std::isfinite(snapshot.referenceHeightMm)) {
    doc["water_reference_height_mm"] = snapshot.referenceHeightMm;
  }
  if (fresh) doc["water_distance_mm"] = snapshot.distanceMm;
  if (levelValid) doc["water_level_mm"] = levelMm;
}

void registerRoutes(WebServer& server) {
  // LocalSensor::registerRoutes owns the single collectHeaders call, including
  // X-EFB-Request, Content-Type and Content-Length. Do not replace that list.
  WebServer* serverPtr = &server;
  server.on("/api/local-water", HTTP_GET,
            [serverPtr]() { sendSnapshot(*serverPtr); });
  server.on("/api/local-water/reference", HTTP_POST,
            [serverPtr]() { handleReferenceUpdate(*serverPtr); });
}

}  // namespace LocalWaterSensor

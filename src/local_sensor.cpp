#include "local_sensor.h"
#include "local_uv_sensor.h"
#include <Adafruit_BME680.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace LocalSensor {
namespace {

// WeAct ESP32-S3: use the printed GPIO labels, not header positions.
constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;
constexpr uint32_t SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t SENSOR_RETRY_MS = 5000;
constexpr uint32_t FRESH_SAMPLE_MS = 3500;

// RAM-only altitude reference: resets to standard pressure after every restart.
// Only the WebServer/main task accesses this value, never the sampling task.
// Changing it never changes measured station pressure or temperature.
constexpr float DEFAULT_REFERENCE_PRESSURE_HPA = 1013.25f;
constexpr double MIN_REFERENCE_PRESSURE_HPA = 800.0;
constexpr double MAX_REFERENCE_PRESSURE_HPA = 1100.0;
constexpr size_t MAX_REFERENCE_TEXT_LENGTH = 16;
constexpr size_t MAX_REFERENCE_REQUEST_BYTES = 96;
float referencePressureHpa = DEFAULT_REFERENCE_PRESSURE_HPA;

struct SensorSnapshot {
  float temperatureC = NAN;
  float pressureHpa = NAN;
  float humidityPct = NAN;
  uint32_t gasResistanceOhms = 0;
  uint32_t capturedAt = 0;
  uint32_t sequence = 0;
  bool hasSample = false;
  bool sensorReady = false;
  const char* status = "Waiting for the BME680.";
};

// Only the sampling task accesses the sensor or I2C bus. HTTP handlers only copy
// this small snapshot under the mutex, then format/send JSON after releasing it.
Adafruit_BME680 environmentalSensor(&Wire);
SensorSnapshot sharedSnapshot;
SemaphoreHandle_t snapshotMutex = nullptr;
bool started = false;

void publishSnapshot(const SensorSnapshot& snapshot) {
  if (snapshotMutex == nullptr) return;
  if (xSemaphoreTake(snapshotMutex, portMAX_DELAY) == pdTRUE) {
    sharedSnapshot = snapshot;
    xSemaphoreGive(snapshotMutex);
  }
}

SensorSnapshot readSnapshot() {
  SensorSnapshot result;
  if (snapshotMutex == nullptr) {
    result.status = "Local sensor unavailable: synchronization not initialized.";
    return result;
  }
  if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    result.status = "Local sensor unavailable: snapshot busy.";
    return result;
  }
  result = sharedSnapshot;
  xSemaphoreGive(snapshotMutex);
  return result;
}

bool addressResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void reportFailure(SensorSnapshot& snapshot, const char* status) {
  snapshot.sensorReady = false;
  snapshot.status = status;  // All callers supply fixed diagnostic literals.
  publishSnapshot(snapshot);
  Serial.println(status);
}

bool initializeSensor(SensorSnapshot& snapshot, uint8_t& sensorAddress) {
  const uint8_t addresses[] = {0x76, 0x77};
  for (uint8_t address : addresses) {
    if (!addressResponds(address)) continue;
    if (!environmentalSensor.begin(address, false)) continue;

    // Explicitly configure forced measurements, checking every setter.
    // The heater is internal to the gas sensor, not an ambient temperature target.
    if (!environmentalSensor.setTemperatureOversampling(BME680_OS_8X) ||
        !environmentalSensor.setPressureOversampling(BME680_OS_4X) ||
        !environmentalSensor.setHumidityOversampling(BME680_OS_2X) ||
        !environmentalSensor.setIIRFilterSize(BME680_FILTER_SIZE_3) ||
        !environmentalSensor.setODR(BME68X_ODR_NONE) ||
        !environmentalSensor.setGasHeater(320, 150)) {
      continue;
    }

    sensorAddress = address;
    snapshot.sensorReady = true;
    snapshot.hasSample = false;  // Never revive a pre-reconnection measurement.
    snapshot.temperatureC = NAN;
    snapshot.pressureHpa = NAN;
    snapshot.humidityPct = NAN;
    snapshot.gasResistanceOhms = 0;
    snapshot.capturedAt = 0;
    snapshot.status = "Sensor initialized; waiting for a fresh sample.";
    publishSnapshot(snapshot);
    Serial.printf("BME680 initialized at 0x%02X.\n", address);
    return true;
  }

  reportFailure(snapshot,
                "BME680 not ready at 0x76/0x77. Check VCC, GND, SDA=8 and SCL=9 with power off.");
  return false;
}

bool validReading(float pressureHpa, float temperatureC, float humidityPct) {
  return std::isfinite(pressureHpa) && std::isfinite(temperatureC) &&
         std::isfinite(humidityPct) &&
         pressureHpa >= 300.0f && pressureHpa <= 1100.0f &&
         temperatureC >= -40.0f && temperatureC <= 85.0f &&
         humidityPct >= 0.0f && humidityPct <= 100.0f;
}

float altitudeFeet(float pressureHpa, float referenceHpa) {
  if (!std::isfinite(pressureHpa) || pressureHpa <= 0.0f ||
      !std::isfinite(referenceHpa) || referenceHpa <= 0.0f) {
    return NAN;
  }
  return 44330.0f *
         (1.0f - std::pow(pressureHpa / referenceHpa, 0.1903f)) /
         0.3048f;
}

bool parseReferencePressure(const char* text, size_t length, float& result) {
  if (text == nullptr || length == 0 || length > MAX_REFERENCE_TEXT_LENGTH) {
    return false;
  }

  // A finite, unsigned decimal with digits on both sides of any decimal point.
  // Reject exponent/hex notation, signs, spaces and embedded NUL characters.
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
      value < MIN_REFERENCE_PRESSURE_HPA || value > MAX_REFERENCE_PRESSURE_HPA) {
    return false;
  }

  // Check double-precision bounds before converting, avoiding float round-up
  // that could incorrectly accept e.g. 799.999999 hPa as exactly 800 hPa.
  result = static_cast<float>(value);
  return true;
}

void samplingTask(void*) {
  SensorSnapshot snapshot;
  bool wireReady = false;
  uint8_t sensorAddress = 0;

  for (;;) {
    if (!wireReady) {
      wireReady = Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, 100000);
      Wire.setTimeOut(50);
      if (!wireReady) {
        reportFailure(snapshot,
                      "I2C initialization failed. Retrying in five seconds.");
        vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
        continue;
      }
    }

    if (!initializeSensor(snapshot, sensorAddress)) {
      vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
      continue;
    }

    TickType_t sampleWake = xTaskGetTickCount();
    while (snapshot.sensorReady) {
      vTaskDelayUntil(&sampleWake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
      if (!addressResponds(sensorAddress)) {
        reportFailure(snapshot,
                      "BME680 disconnected. Check wiring with power off.");
        break;
      }
      // Clear public fields first: library 2.0.6 can return success with no
      // fields, otherwise leaving its previous values in place. Never publish
      // that old sample as fresh. The finite measurement wait occurs only in
      // this background task, not in a WebServer handler.
      environmentalSensor.temperature = NAN;
      environmentalSensor.humidity = NAN;
      environmentalSensor.pressure = 0;
      environmentalSensor.gas_resistance = 0;
      if (!environmentalSensor.performReading()) {
        reportFailure(snapshot,
                      "Sensor read failed. Retrying initialization.");
        break;
      }

      // BME680 library pressure is Pa, unlike the previous BMP5xx driver.
      const float pressureHpa = environmentalSensor.pressure / 100.0f;
      const float temperatureC = environmentalSensor.temperature;
      const float humidityPct = environmentalSensor.humidity;
      if (!validReading(pressureHpa, temperatureC, humidityPct)) {
        reportFailure(snapshot,
                      "Sensor sample outside its operating range; discarded.");
        break;
      }

      snapshot.temperatureC = temperatureC;
      snapshot.pressureHpa = pressureHpa;
      snapshot.humidityPct = humidityPct;
      // Zero means unavailable. Nonzero is raw library output only: v2.0.6
      // does not expose both gas validity/heater-stability flags separately.
      // It must never be presented as a calibrated IAQ or CO2 measurement.
      snapshot.gasResistanceOhms = environmentalSensor.gas_resistance;
      snapshot.capturedAt = millis();
      ++snapshot.sequence;
      snapshot.hasSample = true;
      snapshot.status = "Live sensor readings.";
      publishSnapshot(snapshot);
    }

    vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
  }
}

void fillSnapshot(JsonObject doc) {
  const SensorSnapshot snapshot = readSnapshot();
  const float activeReferenceHpa = referencePressureHpa;
  // Read time after copying so a simultaneously published sample cannot have a
  // timestamp newer than 'now'. Unsigned subtraction also handles millis wrap.
  const uint32_t now = millis();
  const uint32_t sampleAge = now - snapshot.capturedAt;
  const bool fresh = snapshot.sensorReady && snapshot.hasSample &&
                     sampleAge <= FRESH_SAMPLE_MS;
  const char* status = snapshot.status;
  if (snapshot.sensorReady && snapshot.hasSample && !fresh) {
    status = "Sensor data is stale; waiting for a new sample.";
  }

  doc["ok"] = true;  // The request succeeded; sensor health is separate.
  doc["sensor_model"] = "BME680";
  doc["sensor_ok"] = fresh;
  doc["sensor_status"] = status;
  doc["sequence"] = snapshot.sequence;
  doc["sample_age_ms"] = nullptr;
  doc["temperature_c"] = nullptr;
  doc["station_pressure_hpa"] = nullptr;
  doc["relative_humidity_pct"] = nullptr;
  doc["gas_resistance_ohms"] = nullptr;
  doc["gas_available"] = false;
  doc["altitude_ft"] = nullptr;
  if (snapshot.hasSample) doc["sample_age_ms"] = sampleAge;
  if (fresh) {
    doc["temperature_c"] = snapshot.temperatureC;
    doc["station_pressure_hpa"] = snapshot.pressureHpa;
    doc["relative_humidity_pct"] = snapshot.humidityPct;
    if (snapshot.gasResistanceOhms > 0) {
      doc["gas_resistance_ohms"] = snapshot.gasResistanceOhms;
      doc["gas_available"] = true;
    }
    const float altitude = altitudeFeet(snapshot.pressureHpa, activeReferenceHpa);
    if (std::isfinite(altitude)) doc["altitude_ft"] = altitude;
  }
  doc["reference_pressure_hpa"] = activeReferenceHpa;
  doc["uptime_ms"] = now;
  // A separate task and controller supply UV, independent of BME680 health.
  LocalUvSensor::appendSnapshot(doc);
}

void sendSnapshot(WebServer& server) {
  JsonDocument doc;
  fillSnapshot(doc.to<JsonObject>());

  String body;
  body.reserve(1024);
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(200, "application/json; charset=utf-8", body);
}

void sendReferenceError(WebServer& server, int statusCode, const char* message) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = message;  // Callers use fixed strings, never submitted content.
  String body;
  serializeJson(doc, body);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(statusCode, "application/json; charset=utf-8", body);
}

void handleReferenceUpdate(WebServer& server) {
  // No CORS response is granted: cross-origin browser scripts cannot supply
  // this custom header without a successful preflight. This is not user auth.
  if (server.header("X-EFB-Request") != "1") {
    sendReferenceError(server, 403, "Use the altitude reference control on this device.");
    return;
  }

  String contentType = server.header("Content-Type");
  const int separator = contentType.indexOf(';');
  if (separator >= 0) contentType = contentType.substring(0, separator);
  contentType.trim();
  if (!contentType.equalsIgnoreCase("application/x-www-form-urlencoded")) {
    sendReferenceError(server, 415, "Send a form-encoded altitude reference.");
    return;
  }

  // WebServer has already parsed the request body before this handler runs.
  // These checks bound what this endpoint accepts, not the parser allocation.
  const String lengthText = server.header("Content-Length");
  size_t requestBytes = 0;
  if (lengthText.length() == 0 || lengthText.length() > 3) {
    sendReferenceError(server, 400, "Invalid altitude reference request size.");
    return;
  }
  for (size_t i = 0; i < lengthText.length(); ++i) {
    const char ch = lengthText[i];
    if (ch < '0' || ch > '9') {
      sendReferenceError(server, 400, "Invalid altitude reference request size.");
      return;
    }
    requestBytes = requestBytes * 10 + static_cast<size_t>(ch - '0');
  }
  if (requestBytes == 0 || requestBytes > MAX_REFERENCE_REQUEST_BYTES) {
    sendReferenceError(server, 413, "Altitude reference request must be 1 to 96 bytes.");
    return;
  }

  if (server.args() != 1 || server.argName(0) != "reference_pressure_hpa") {
    sendReferenceError(server, 400, "Submit only reference_pressure_hpa.");
    return;
  }

  const String text = server.arg("reference_pressure_hpa");
  float newReferenceHpa = 0.0f;
  if (!parseReferencePressure(text.c_str(), text.length(), newReferenceHpa)) {
    sendReferenceError(server, 400, "Reference must be a decimal from 800 to 1100 hPa.");
    return;
  }

  referencePressureHpa = newReferenceHpa;
  // An offline sensor does not prevent updating the reference. Its readings
  // remain null until a fresh sample arrives, while the new reference is echoed.
  sendSnapshot(server);
}

}  // namespace

void appendSnapshot(JsonObject doc) {
  fillSnapshot(doc);
}

void begin() {
  if (started) return;
  started = true;
  // Start UV even if the BME680 mutex/task cannot be created.
  LocalUvSensor::begin();

  snapshotMutex = xSemaphoreCreateMutex();
  if (snapshotMutex == nullptr) {
    Serial.println("Local sensor unavailable: could not create snapshot mutex.");
    return;
  }

  // ESP32 FreeRTOS specifies this stack depth in bytes. A core-1 background task
  // keeps sampling while the normal loop waits for HTTPS weather requests.
  if (xTaskCreatePinnedToCore(samplingTask, "efb-bme680", 4096, nullptr, 1,
                              nullptr, 1) != pdPASS) {
    SensorSnapshot failed;
    failed.status = "Local sensor unavailable: could not start sampling task.";
    publishSnapshot(failed);
    Serial.println(failed.status);
    return;
  }
  Serial.println("Local BME680 task started: SDA=8, SCL=9; ESP32-S3 board.");
}

void registerRoutes(WebServer& server) {
  // This project has no other collectHeaders call. Include future required
  // headers here too: WebServer replaces (rather than appends to) this list.
  const char* requestHeaders[] = {
    "X-EFB-Request", "Content-Type", "Content-Length"
  };
  server.collectHeaders(requestHeaders,
                        sizeof(requestHeaders) / sizeof(requestHeaders[0]));
  WebServer* serverPtr = &server;
  server.on("/api/local-sensor", HTTP_GET,
            [serverPtr]() { sendSnapshot(*serverPtr); });
  server.on("/api/local-sensor/reference", HTTP_POST,
            [serverPtr]() { handleReferenceUpdate(*serverPtr); });
}

}

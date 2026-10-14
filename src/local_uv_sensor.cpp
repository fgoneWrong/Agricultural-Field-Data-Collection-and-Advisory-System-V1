#include "local_uv_sensor.h"

#include <Adafruit_LTR390.h>
#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace LocalUvSensor {
namespace {

// Separate hardware controller: never reconfigure BME680's Wire on GPIO8/9.
// These are printed GPIO numbers, not physical header positions.
constexpr int UV_SDA_PIN = 16;
constexpr int UV_SCL_PIN = 17;
constexpr uint8_t UV_ADDRESS = 0x53;
constexpr uint32_t UV_I2C_CLOCK_HZ = 100000;
constexpr uint16_t UV_I2C_TIMEOUT_MS = 50;
constexpr uint32_t SENSOR_RETRY_MS = 5000;
constexpr uint32_t FRESH_SAMPLE_MS = 3500;
constexpr uint32_t POLL_INTERVAL_MS = 250;
constexpr uint32_t NO_DATA_TIMEOUT_MS = 3000;

// Lite-On LTR-390UV-01 V1.2, section 4.5 (Waveshare UV Sensor (C) datasheet):
// 1400 counts/UVI at 18x gain, 20-bit resolution, NO optical window.
// https://files.waveshare.com/upload/a/ac/1_LTR-390UV_Final_DS_V1_2.pdf
// This is a datasheet-based estimate, not calibration against a reference meter.
constexpr float UV_COUNTS_PER_INDEX = 1400.0f;
constexpr uint32_t MAX_UV_COUNTS = 0xFFFFF;
// 20-bit conversion needs 400ms; register rate=5 selects one measurement/1000ms.
constexpr uint8_t UV_MEAS_RATE_VALUE = 0x05;
constexpr uint8_t UV_MAIN_CTRL_VALUE = 0x0A;  // UV mode + enable.

struct UvSnapshot {
  uint32_t rawCounts = 0;
  uint32_t capturedAt = 0;
  uint32_t sequence = 0;
  bool hasSample = false;
  bool sensorReady = false;
  const char* status = "Waiting for the LTR390.";
};

// Only the UV task touches this controller or the Adafruit driver.
TwoWire& uvWire = Wire1;
Adafruit_LTR390 uvSensor;
bool libraryBeginAttempted = false;
UvSnapshot sharedSnapshot;
SemaphoreHandle_t snapshotMutex = nullptr;
bool started = false;

void publishSnapshot(const UvSnapshot& snapshot) {
  if (snapshotMutex == nullptr) return;
  if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    sharedSnapshot = snapshot;
    xSemaphoreGive(snapshotMutex);
  }
}

UvSnapshot readSnapshot() {
  UvSnapshot snapshot;
  if (snapshotMutex == nullptr) {
    snapshot.status = "UV unavailable: synchronization not initialized.";
    return snapshot;
  }
  if (xSemaphoreTake(snapshotMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    snapshot.status = "UV unavailable: snapshot busy.";
    return snapshot;
  }
  snapshot = sharedSnapshot;
  xSemaphoreGive(snapshotMutex);
  return snapshot;
}

// Adafruit readUVS()/newDataAvailable() do not expose transfer errors. Checked
// Wire reads preserve the important distinction between a real zero and a
// failed/disconnected sensor. A single burst keeps all three ADC bytes coherent.
bool readRegisters(uint8_t reg, uint8_t* bytes, size_t count) {
  uvWire.beginTransmission(UV_ADDRESS);
  const bool queued = uvWire.write(reg) == 1;
  if (!queued) {
    uvWire.endTransmission(true);
    return false;
  }
  // Keep the bus for the documented register-address + repeated-START read.
  if (uvWire.endTransmission(false) != 0) return false;
  if (uvWire.requestFrom(UV_ADDRESS, count, true) != count) return false;
  for (size_t i = 0; i < count; ++i) {
    const int value = uvWire.read();
    if (value < 0) return false;
    bytes[i] = static_cast<uint8_t>(value);
  }
  return true;
}

bool writeRegister(uint8_t reg, uint8_t value) {
  uvWire.beginTransmission(UV_ADDRESS);
  const bool registerQueued = uvWire.write(reg) == 1;
  const bool valueQueued = uvWire.write(value) == 1;
  const uint8_t result = uvWire.endTransmission();
  return registerQueued && valueQueued && result == 0;
}

bool configurationMatches() {
  uint8_t control = 0;
  uint8_t config[3] = {};  // Rate, gain, part ID are adjacent registers.
  return readRegisters(LTR390_MAIN_CTRL, &control, 1) &&
         readRegisters(LTR390_MEAS_RATE, config, sizeof(config)) &&
         control == UV_MAIN_CTRL_VALUE &&
         config[0] == UV_MEAS_RATE_VALUE &&
         config[1] == static_cast<uint8_t>(LTR390_GAIN_18) &&
         (config[2] >> 4) == 0x0B;
}

bool decodeRawCounts(const uint8_t* bytes, uint32_t& result) {
  const uint32_t counts = static_cast<uint32_t>(bytes[0]) |
                          (static_cast<uint32_t>(bytes[1]) << 8) |
                          (static_cast<uint32_t>(bytes[2]) << 16);
  // Upper bits are reserved. Full scale is saturation, not a reliable UVI.
  if (counts >= MAX_UV_COUNTS) return false;
  result = counts;  // Zero is a valid successfully transferred dark reading.
  return true;
}

void reportFailure(UvSnapshot& snapshot, const char* status) {
  snapshot.sensorReady = false;
  snapshot.status = status;
  publishSnapshot(snapshot);
  Serial.println(status);
}

bool initializeSensor(UvSnapshot& snapshot) {
  uint8_t partId = 0;
  if (!readRegisters(LTR390_PART_ID, &partId, 1) || (partId >> 4) != 0x0B) {
    reportFailure(snapshot,
                  "LTR390 not ready at 0x53. Check VCC, GND, SDA=16 and SCL=17 with power off.");
    return false;
  }

  // Adafruit LTR390 1.1.2 allocates private objects on every begin() and has no
  // corresponding cleanup. Invoke it at most once per boot, after an ID check.
  // Even if that call fails, its I2CDevice already exists; subsequent recovery
  // uses only the driver's register setters, never its uninitialized status
  // pointers. No new allocation is needed for a missing or reconnected sensor.
  if (!libraryBeginAttempted) {
    libraryBeginAttempted = true;
    const bool libraryReady = uvSensor.begin(&uvWire);
    uvWire.setTimeOut(UV_I2C_TIMEOUT_MS);
    if (!libraryReady) {
      reportFailure(snapshot,
                    "LTR390 initial setup failed; retrying checked configuration.");
      return false;
    }
  }

  // Reset only configuration, not the shared ESP32 or BME680 controller. Disable
  // while configuring so no old/differently scaled sample can be published.
  if (!writeRegister(LTR390_MAIN_CTRL, 0x00)) {
    reportFailure(snapshot, "LTR390 configuration write failed.");
    return false;
  }
  uvSensor.setMode(LTR390_MODE_UVS);
  uvSensor.setGain(LTR390_GAIN_18);
  uvSensor.setResolution(LTR390_RESOLUTION_20BIT);
  // The library does not provide a measurement-rate setter.
  if (!writeRegister(LTR390_MEAS_RATE, UV_MEAS_RATE_VALUE)) {
    reportFailure(snapshot, "LTR390 measurement-rate write failed.");
    return false;
  }
  uvSensor.enable(true);
  if (!configurationMatches()) {
    reportFailure(snapshot, "LTR390 configuration verification failed.");
    return false;
  }

  // Clear power-on/data-ready status and discard any pre-reconnection data.
  uint8_t status = 0;
  uint8_t discard[3] = {};
  if (!readRegisters(LTR390_MAIN_STATUS, &status, 1) ||
      !readRegisters(LTR390_UVSDATA, discard, sizeof(discard))) {
    reportFailure(snapshot, "LTR390 initial status read failed.");
    return false;
  }
  snapshot.sensorReady = true;
  snapshot.hasSample = false;
  snapshot.capturedAt = 0;
  snapshot.rawCounts = 0;
  snapshot.status = "LTR390 initialized; waiting for a fresh UV sample.";
  publishSnapshot(snapshot);
  Serial.println("LTR390 initialized: I2C1 SDA=16 SCL=17, address=0x53, 18x/20-bit.");
  return true;
}

void samplingTask(void*) {
  UvSnapshot snapshot;
  for (;;) {
    // Recover this controller after communication failures, never Wire/BME680.
    uvWire.end();
    const bool busReady = uvWire.begin(UV_SDA_PIN, UV_SCL_PIN, UV_I2C_CLOCK_HZ);
    uvWire.setTimeOut(UV_I2C_TIMEOUT_MS);
    if (!busReady) {
      reportFailure(snapshot, "LTR390 I2C1 initialization failed; retrying.");
      vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
      continue;
    }
    if (!initializeSensor(snapshot)) {
      vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
      continue;
    }

    uint32_t lastDataAt = millis();
    while (snapshot.sensorReady) {
      vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
      uint8_t status = 0;
      if (!configurationMatches() ||
          !readRegisters(LTR390_MAIN_STATUS, &status, 1)) {
        reportFailure(snapshot, "LTR390 disconnected or configuration changed; retrying.");
        break;
      }
      if ((status & 0x20) != 0 || (status & 0xC7) != 0) {
        reportFailure(snapshot, "LTR390 restarted or invalid status received; retrying.");
        break;
      }
      if ((status & 0x08) == 0) {
        if (millis() - lastDataAt > NO_DATA_TIMEOUT_MS) {
          reportFailure(snapshot, "LTR390 stopped producing new UV samples; retrying.");
        }
        continue;
      }

      uint8_t bytes[3] = {};
      uint32_t rawCounts = 0;
      if (!readRegisters(LTR390_UVSDATA, bytes, sizeof(bytes))) {
        reportFailure(snapshot, "LTR390 UV read failed; retrying.");
        break;
      }
      if (!decodeRawCounts(bytes, rawCounts)) {
        reportFailure(snapshot, "LTR390 reading saturated or invalid; UV unavailable.");
        break;
      }
      snapshot.rawCounts = rawCounts;
      snapshot.capturedAt = millis();
      lastDataAt = snapshot.capturedAt;
      ++snapshot.sequence;
      snapshot.hasSample = true;
      snapshot.status = "Live LTR390 UV index estimate.";
      publishSnapshot(snapshot);
    }
    vTaskDelay(pdMS_TO_TICKS(SENSOR_RETRY_MS));
  }
}

}  // namespace

void begin() {
  if (started) return;
  started = true;
  snapshotMutex = xSemaphoreCreateMutex();
  if (snapshotMutex == nullptr) {
    Serial.println("Local UV unavailable: could not create snapshot mutex.");
    return;
  }
  if (xTaskCreatePinnedToCore(samplingTask, "efb-ltr390", 4096, nullptr, 1,
                            nullptr, 1) != pdPASS) {
    UvSnapshot failed;
    failed.status = "Local UV unavailable: could not start sampling task.";
    publishSnapshot(failed);
    Serial.println(failed.status);
    return;
  }
  Serial.println("Local LTR390 task started: separate I2C1 SDA=16, SCL=17.");
}

void appendSnapshot(JsonObject doc) {
  const UvSnapshot snapshot = readSnapshot();
  // Copy first, then get time: avoid subtraction from a future sample timestamp.
  const uint32_t now = millis();
  const uint32_t sampleAge = now - snapshot.capturedAt;
  const bool fresh = snapshot.sensorReady && snapshot.hasSample &&
                     sampleAge <= FRESH_SAMPLE_MS;
  const char* status = snapshot.status;
  if (snapshot.sensorReady && snapshot.hasSample && !fresh) {
    status = "UV data is stale; waiting for a new sample.";
  }
  doc["uv_sensor_model"] = "LTR390";
  doc["uv_sensor_ok"] = fresh;
  doc["uv_sensor_status"] = status;
  doc["uv_sequence"] = snapshot.sequence;
  doc["uv_sample_age_ms"] = nullptr;
  doc["uv_index_estimate"] = nullptr;
  doc["uv_raw_counts"] = nullptr;
  if (snapshot.hasSample) doc["uv_sample_age_ms"] = sampleAge;
  if (fresh) {
    doc["uv_raw_counts"] = snapshot.rawCounts;
    doc["uv_index_estimate"] = snapshot.rawCounts / UV_COUNTS_PER_INDEX;
  }
}

}  // namespace LocalUvSensor

#pragma once

#include <WebServer.h>
#include <ArduinoJson.h>

namespace LocalSensor {

// Start independent BME680 and LTR390 sampling tasks. Call once from setup().
// Fermion SEN0375: VCC = ESP32 3V3, GND, SDA = GPIO8, SCL = GPIO9.
// Probe default I2C address 0x76, then alternate 0x77. No BMP581 is used.
// LTR390: separate I2C controller 1, SDA = GPIO16, SCL = GPIO17, address 0x53.
// Its failure/staleness never invalidates otherwise valid BME680 readings.
void begin();

// Copy fresh BME680/LTR390 data into a JSON object (no sensor I/O).
void appendSnapshot(JsonObject doc);

// Add GET /api/local-sensor and POST /api/local-sensor/reference. The POST
// accepts only the form field reference_pressure_hpa (800-1100 hPa), with
// X-EFB-Request: 1. It adjusts estimated altitude, not measured pressure.
// The reference lives in RAM and resets to 1013.25 hPa when the board restarts.
// Registers the request headers needed by POST; add other required headers to
// the same collection list rather than replacing it with a second call.
// The supplied server must outlive its registered routes, as with other routes.
void registerRoutes(WebServer& server);

}  // namespace LocalSensor

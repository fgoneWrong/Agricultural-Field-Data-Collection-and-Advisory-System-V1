#pragma once

#include <ArduinoJson.h>
#include <WebServer.h>

// JSN-SR04T-V3.3 in default Mode 0. Only its background task touches GPIOs.
namespace LocalWaterSensor {
void begin();
void appendSnapshot(JsonObject doc);
void registerRoutes(WebServer& server);
}

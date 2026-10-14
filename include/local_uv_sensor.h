#pragma once

#include <ArduinoJson.h>

// Independent LTR390 acquisition. No HTTP handler performs an I2C operation.
namespace LocalUvSensor {
void begin();
void appendSnapshot(JsonObject doc);
}

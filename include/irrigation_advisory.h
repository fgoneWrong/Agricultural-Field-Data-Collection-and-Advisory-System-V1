#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// HTTPS runs only in this module's worker, never in an HTTP handler or sensor
// task. No irrigation hardware is controlled. Call begin() once during setup.
namespace IrrigationAdvisory {
void begin();
// Prevent overlapping TLS weather transfers while the Groq worker owns memory.
bool busy();
// Copies bounded, already validated context; returns immediately after enqueue.
// A successful enqueue is not a completed recommendation. Poll appendStatus().
bool request(const JsonDocument& context, String& error);
// Cached snapshot only. The decision is ALWAYS review_required, with no duration.
void appendStatus(JsonObject doc);
}

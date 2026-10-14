#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bangladesh_upazila_coordinates.h"
#include "local_sensor.h"
#include "local_water_sensor.h"
#include "irrigation_advisory.h"
#include "field_extensions_ui.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "V1.7 now requires ESP32-S3. Select the esp32-s3-n16r2 environment."
#endif

// HTTPS/TLS and WebServer handlers need more than Arduino's default 8 KB
// loopTask stack. This ESP32 has sufficient RAM for a safer 16 KB task.
SET_LOOP_TASK_STACK_SIZE(16384);

// Leave the S3 board's addressable RGB LED unused; digitalWrite cannot drive it.
// -1 disables the optional time-sync LED without claiming any GPIO.
constexpr int STATUS_LED_PIN = -1;

// Wi-Fi settings
const char* HOME_WIFI_SSID = "Agromet";
const char* HOME_WIFI_PASSWORD = "agro@met";

// Bangladesh time (UTC+6, no daylight-saving time).
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_SECONDS = 6L * 3600L;
const int DAYLIGHT_OFFSET_SECONDS = 0;

WebServer server(80);

// Keep TLS clients and large request scratch buffers out of that stack.
WiFiClientSecure weatherSecureClient;
HTTPClient weatherHttp;

constexpr uint8_t HISTORY_DAYS = 7;
constexpr uint16_t HOURLY_CHART_POINTS = HISTORY_DAYS * 24;
constexpr int16_t HOURLY_CHART_MISSING = INT16_MIN;
constexpr uint8_t HOURLY_HUMIDITY_MISSING = 255;
constexpr size_t MAX_LOG_RECORDS = 100;
constexpr size_t DISTRICT_TEXT_LENGTH = 32;
constexpr size_t UPAZILA_TEXT_LENGTH = 48;
constexpr size_t BLOCK_TEXT_LENGTH = 32;
constexpr size_t PLOT_ID_TEXT_LENGTH = 40;
constexpr size_t CROP_TEXT_LENGTH = 48;
constexpr size_t VARIETY_TEXT_LENGTH = 48;
constexpr size_t SOIL_TYPE_TEXT_LENGTH = 24;
// Software input bound (1,000 hectares), not an agronomic recommendation.
constexpr double MAX_PLOT_AREA_M2 = 10000000.0;
const char* const SOIL_TYPE_OPTIONS[] = {
  "Sand", "Loamy sand", "Sandy loam", "Loam", "Silt loam", "Silt",
  "Sandy clay loam", "Clay loam", "Silty clay loam", "Sandy clay",
  "Silty clay", "Clay", "Unknown"
};
constexpr size_t TIMESTAMP_TEXT_LENGTH = 24;
constexpr unsigned long WEATHER_CACHE_DURATION_MS = 15UL * 60UL * 1000UL;
constexpr uint16_t WEATHER_HTTP_TIMEOUT_MS = 8000;

enum class HistorySource : uint8_t {
  HistoricalForecast,
  ForecastFallback
};

// The previous seven completed calendar days do not use a weather condition
// label. A daily WMO code is only the model's most severe category for a day,
// which made the old history misleading for Bangladesh.
struct DailyWeather {
  char date[11];                 // YYYY-MM-DD
  float rainfallMm;
  float precipitationHours;
  float uvIndexMax;
  float temperatureMinC;
  float temperatureMaxC;
};

struct WeatherData {
  // Selected Upazila reference coordinate.
  float latitude;
  float longitude;
  CoordinateBasis coordinateBasis;

  // Grid-cell coordinates returned by the individual weather services.
  float forecastGridLatitude;
  float forecastGridLongitude;
  float historyGridLatitude;
  float historyGridLongitude;

  // Current forecast-model values.
  float currentTemperatureC;
  float relativeHumidityPercent;
  float cloudCoverPercent;
  float currentPrecipitationMm;
  float currentUvIndex;
  int16_t currentWeatherCode;
  char reportedAt[TIMESTAMP_TEXT_LENGTH];

  // Previous seven fully completed Bangladesh calendar days.
  float previous7CompletedDaysRainfallMm;
  HistorySource historySource;
  DailyWeather history[HISTORY_DAYS];
};

struct HourlyChartSample {
  // Compact tenths preserve useful precision while keeping the chart cache
  // small. Missing sentinels are emitted as JSON null should a provider ever
  // return a gap.
  int16_t rainfallTenthsMm;
  int16_t temperatureTenthsC;
  int16_t uvIndexTenths;
  uint8_t relativeHumidityPercent;
};

// This chart cache is intentionally separate from FieldData. Storing 168
// hourly points in every saved record would waste RAM; one current-location
// chart cache is enough for the phone interface.
struct HourlyChartData {
  char startDate[11];            // YYYY-MM-DD in Bangladesh time
  char fetchedAt[TIMESTAMP_TEXT_LENGTH];
  float gridLatitude;
  float gridLongitude;
  HistorySource source;
  HourlyChartSample samples[HOURLY_CHART_POINTS];
};

struct FieldData {
  uint32_t recordId;
  char district[DISTRICT_TEXT_LENGTH];
  char upazila[UPAZILA_TEXT_LENGTH];
  char savedAt[TIMESTAMP_TEXT_LENGTH];

  // Manual field-visit values.
  char block[BLOCK_TEXT_LENGTH];
  char plotId[PLOT_ID_TEXT_LENGTH];
  double plotAreaM2;
  char crop[CROP_TEXT_LENGTH];
  char variety[VARIETY_TEXT_LENGTH];
  // "Unknown" explicitly means missing soil information for future advice.
  char soilType[SOIL_TYPE_TEXT_LENGTH];
  float irrigationWaterLevelMm;
  float tensiometerCb;
  bool pumpOn;
  bool waterFromSensor;
  float waterSensorDistanceMm;
  float waterReferenceHeightMm;
  float waterSensorLevelMm;

  // Automatic weather values fetched by the ESP32, not trusted from the phone.
  WeatherData weather;
};

uint32_t nextRecordId = 1;
FieldData logRecords[MAX_LOG_RECORDS];
size_t logStart = 0;
size_t logCount = 0;

WeatherData cachedWeather;
int cachedUpazilaIndex = -1;
unsigned long weatherCacheUpdatedAt = 0;
bool weatherCacheValid = false;

HourlyChartData cachedHourlyChart;
int cachedHourlyChartUpazilaIndex = -1;
unsigned long hourlyChartCacheUpdatedAt = 0;
bool hourlyChartCacheValid = false;

HourlyChartData weatherFetchHourlyChart;
WeatherData weatherResponseScratch;
FieldData submittedRecordScratch;

unsigned long lastReconnectAttempt = 0;

const char htmlPage[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Agro-advisory Field Toolkit</title>
  <style>
    * { box-sizing: border-box; }
    body {
      margin: 0;
      padding: 24px 15px;
      font-family: Arial, sans-serif;
      background: #f2f7f2;
      color: #193b24;
    }
    .card {
      max-width: 560px;
      margin: auto;
      padding: 25px;
      background: #ffffff;
      border-radius: 15px;
      box-shadow: 0 4px 18px rgba(0, 0, 0, .15);
    }
    h1 { margin: 0 0 6px; text-align: center; color: #167342; }
    .intro { margin: 0 0 24px; text-align: center; color: #607060; }
    .section-title {
      margin: 30px 0 12px;
      padding-bottom: 6px;
      border-bottom: 2px solid #dbe8dc;
      color: #167342;
      font-size: 19px;
      font-weight: bold;
    }
    label { display: block; margin: 16px 0 6px; font-weight: bold; }
    input, select, button {
      width: 100%;
      padding: 13px;
      border: 1px solid #b8c8ba;
      border-radius: 8px;
      font-size: 16px;
    }
    input:focus, select:focus { outline: none; border-color: #167342; }
    .secondary, .download, button {
      display: block;
      width: 100%;
      margin-top: 14px;
      padding: 13px;
      border: 0;
      border-radius: 8px;
      color: #ffffff;
      font-size: 16px;
      font-weight: bold;
      text-align: center;
      text-decoration: none;
      cursor: pointer;
    }
    button { margin-top: 28px; background: #16874a; }
    button:disabled { cursor: not-allowed; opacity: .55; }
    .secondary { background: #2463a6; }
    .download { background: #5d4aa8; }
    .weather-status {
      margin: 12px 0;
      padding: 12px;
      border-radius: 8px;
      background: #edf4ed;
      color: #31573a;
    }
    .weather-status.error { background: #fff0f0; color: #a52d2d; }
    .weather-status.loading { background: #eef5ff; color: #1d5289; }
    .weather-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
      margin-top: 12px;
    }
    .weather-value {
      padding: 12px;
      border-radius: 8px;
      background: #f6faf6;
      border: 1px solid #dbe8dc;
    }
    .weather-value span { display: block; color: #607060; font-size: 13px; }
    .weather-value strong { display: block; margin-top: 4px; color: #193b24; }
    .history-title { margin: 22px 0 8px; font-weight: bold; color: #167342; }
    .weather-subtitle { margin: 20px 0 8px; font-size: 17px; color: #167342; }
    .local-sensor-panel { margin: 14px 0 24px; padding: 16px; border: 1px solid #cdddce; border-radius: 10px; background: #fbfdfb; }
    .local-sensor-panel .weather-subtitle { margin: 0 0 6px; }
    .local-sensor-caption { margin: 0; color: #607060; font-size: 13px; line-height: 1.5; }
    .local-sensor-grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }
    .local-sensor-grid .weather-value strong { font-size: 23px; font-variant-numeric: tabular-nums; overflow-wrap: anywhere; }
    .local-sensor-grid .sensor-unit { display: block; margin-top: 5px; color: #607060; font-size: 13px; }
    .local-sensor-meta { margin: 12px 0 0; color: #607060; font-size: 12px; line-height: 1.6; }
    .sensor-reference-controls { margin-top: 16px; border-top: 1px solid #dbe8dc; padding-top: 14px; }
    .sensor-reference-controls summary { color: #167342; font-weight: bold; cursor: pointer; }
    .sensor-reference-controls summary:focus-visible { outline: 2px solid #167342; outline-offset: 4px; }
    .sensor-reference-actions { display: flex; flex-wrap: wrap; gap: 8px; }
    .sensor-reference-actions button { flex: 1 1 170px; margin-top: 12px; font-size: 14px; }
    #sensorReferenceMessage { margin: 10px 0 0; font-size: 13px; }
    #localSensorStatus { font-size: 13px; }
    @media (max-width: 540px) {
      .local-sensor-grid { grid-template-columns: 1fr; }
      .local-sensor-grid .weather-value { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
      .local-sensor-reading { text-align: right; }
    }
    .history-wrap { overflow-x: auto; }
    .hourly-chart { margin-top: 24px; }
    .chart-heading { display: flex; align-items: flex-start; flex-direction: column; gap: 12px; }
    .chart-heading .history-title { margin: 0; }
    .chart-toggle { display: inline-flex; align-items: center; gap: 7px; width: auto; margin: 0; color: #31573a; font-size: 14px; line-height: 1.5; cursor: pointer; }
    .chart-toggle input { width: auto; margin: 0; padding: 0; }
    .chart-wrap { position: relative; margin-top: 10px; border: 1px solid #dbe8dc; border-radius: 8px; overflow: hidden; background: #f7fbf8; }
    #hourlyChart { display: block; width: 100%; height: 275px; touch-action: pan-y; }
    .chart-wrap.show-extra-series #hourlyChart { height: 400px; }
    .chart-tooltip { position: absolute; z-index: 2; max-width: 205px; padding: 8px 10px; border-radius: 7px; background: #193b24; color: #ffffff; font-size: 12px; line-height: 1.4; pointer-events: none; white-space: pre-line; box-shadow: 0 2px 8px rgba(0, 0, 0, .22); }
    .chart-legend { display: flex; flex-wrap: wrap; gap: 12px; margin-top: 10px; color: #526856; font-size: 13px; }
    .legend-item { display: inline-flex; align-items: center; gap: 5px; }
    .legend-swatch { width: 15px; height: 3px; border-radius: 3px; }
    .legend-rain { background: #1b9eea; height: 9px; }
    .legend-temperature { background: #6548c9; }
    .legend-humidity { background: #149c68; }
    .legend-uv { background: #b76a00; }
    .chart-extra-note { margin: 10px 0 0; color: #607060; font-size: 12px; line-height: 1.5; }
    table { width: 100%; min-width: 430px; border-collapse: collapse; }
    th, td { padding: 9px; border-bottom: 1px solid #dbe8dc; text-align: left; }
    th { color: #167342; font-size: 13px; }
    .pump-options { display: flex; gap: 10px; }
    .pump-option {
      flex: 1;
      margin: 0;
      padding: 12px;
      border: 1px solid #b8c8ba;
      border-radius: 8px;
      text-align: center;
      font-weight: normal;
    }
    .pump-option input { width: auto; margin-right: 6px; }
    .note { margin: 16px 0 0; color: #64756a; font-size: 13px; text-align: center; }
    [hidden] { display: none !important; }
    @media (max-width: 420px) {
      .chart-heading { align-items: flex-start; flex-direction: column; }
      #hourlyChart { height: 255px; }
      .chart-wrap.show-extra-series #hourlyChart { height: 375px; }
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>Agro-advisory Field Toolkit</h1>
    <p class="intro">IoT-Based Agricultural Data Collection and advisory System</p>

    <form id="fieldForm" action="/submit" method="POST">
      <div class="section-title">Field Geographical Location</div>

      <label for="district">District</label>
      <input list="districts" type="search" id="district" name="district"
             placeholder="Search District..." autocomplete="off" required>
      <datalist id="districts"></datalist>

      <label for="upazila">Upazila</label>
      <input list="upazilas" type="search" id="upazila" name="upazila"
             placeholder="Search Upazila..." autocomplete="off" required>
      <datalist id="upazilas"></datalist>

      <div class="section-title">Weather Information</div>
      <section id="localSensorPanel" class="local-sensor-panel" aria-labelledby="localSensorTitle">
        <h2 id="localSensorTitle" class="weather-subtitle">Local Sensor Data</h2>
        <p class="local-sensor-caption">Live BME680 and LTR390 measurements at the device &mdash; separate from the selected location's online weather.</p>
        <div id="localSensorStatus" class="weather-status loading" role="status" aria-live="polite">Connecting to the local sensor...</div>
        <div class="local-sensor-grid">
          <div class="weather-value"><span>Temperature</span><div class="local-sensor-reading"><strong id="localSensorTemperature">&mdash;</strong><span class="sensor-unit">&deg;C</span></div></div>
          <div class="weather-value"><span>Relative humidity</span><div class="local-sensor-reading"><strong id="localSensorHumidity">&mdash;</strong><span class="sensor-unit">% RH</span></div></div>
          <div class="weather-value"><span>Barometric pressure</span><div class="local-sensor-reading"><strong id="localSensorPressure">&mdash;</strong><span class="sensor-unit">hPa</span></div></div>
          <div class="weather-value"><span id="localSensorAltitudeLabel">Pressure altitude (estimate)</span><div class="local-sensor-reading"><strong id="localSensorAltitude">&mdash;</strong><span class="sensor-unit">feet</span></div></div>
          <div class="weather-value"><span>Gas resistance (raw)</span><div class="local-sensor-reading"><strong id="localSensorGas">&mdash;</strong><span class="sensor-unit">k&Omega;</span></div></div>
          <div class="weather-value"><span>UV index (estimate)</span><div class="local-sensor-reading"><strong id="localSensorUv">&mdash;</strong><span class="sensor-unit">LTR390 &middot; unitless</span></div></div>
        </div>
        <p id="localSensorUvStatus" class="local-sensor-meta" role="status" aria-live="polite">LTR390: waiting for a UV measurement.</p>
        <p class="local-sensor-meta">UV is a sensor-derived estimate, not a calibrated reference measurement. Test level and sky-facing, without a glass cover. Keep the BME680 shaded and ventilated.</p>
        <p id="localSensorMeta" class="local-sensor-meta">Waiting for a valid measurement.</p>
        <p class="local-sensor-meta">Active altitude reference: <strong><span id="localSensorReference">&mdash;</span> hPa</strong>. <span id="localSensorReferenceMode">Waiting for the device.</span></p>
        <p class="local-sensor-meta">Pressure is measured locally, not sea-level corrected. Altitude is an estimate, not GPS elevation. Sensor temperature can be affected by its gas heater, ESP32 heat or sunlight.</p>
        <p id="localSensorGasNote" class="local-sensor-meta">Gas resistance is a raw sensor reading, not an IAQ score, CO2 concentration or safety alarm.</p>
        <details class="sensor-reference-controls">
          <summary>Adjust altitude reference</summary>
          <p class="local-sensor-meta" id="sensorReferenceHelp">Enter a current nearby QNH / altimeter setting in hPa (800&ndash;1100). This changes altitude only, not measured pressure or temperature. Refresh the reference as weather changes.</p>
          <label for="sensorReferenceInput">Reference pressure (hPa)</label>
          <input id="sensorReferenceInput" name="reference_pressure_hpa" form="sensorReferenceForm"
                 type="number" min="800" max="1100" step="0.01" inputmode="decimal"
                 placeholder="Example: 1013.25" aria-describedby="sensorReferenceHelp sensorReferenceLifetime" required disabled>
          <div class="sensor-reference-actions">
            <button id="applySensorReference" type="submit" form="sensorReferenceForm" disabled>Apply reference</button>
            <button id="resetSensorReference" class="secondary" type="button" disabled>Reset to 1013.25 hPa</button>
          </div>
          <p id="sensorReferenceLifetime" class="local-sensor-meta">Applies to everyone viewing this ESP32. Resets to 1013.25 hPa on restart. Does not save a field record.</p>
          <p id="sensorReferenceMessage" role="status" aria-live="polite"></p>
        </details>
      </section>
      <section class="local-sensor-panel" aria-labelledby="localWaterTitle">
        <h2 id="localWaterTitle" class="weather-subtitle">Local Water Level &mdash; JSN-SR04T</h2>
        <div class="weather-grid">
          <div class="weather-value"><span>Probe-to-surface distance</span><strong id="localWaterDistance">&mdash;</strong><span>mm</span></div>
          <div class="weather-value"><span>Surface water depth (estimate)</span><strong id="localWaterLevel">&mdash;</strong><span>mm</span></div>
        </div>
        <p id="localWaterStatus" class="weather-status" role="status" aria-live="polite">Waiting for JSN-SR04T...</p>
        <p class="note">This measures an air gap, not underground soil moisture or a subsurface water table. Keep the probe vertical and at least 230 mm above the highest water surface, with extra mounting margin. Invalid echoes are unavailable, never zero.</p>
        <details class="sensor-reference-controls">
          <summary>Set zero-water reference</summary>
          <p class="note">Measure vertically from the probe face to the soil surface / tank bottom used as zero water. Water depth = reference height &minus; measured distance. Active reference: <strong id="waterReferenceValue">Not configured</strong>.</p>
          <label for="waterReferenceInput">Probe-to-zero-water height (mm)</label>
          <input id="waterReferenceInput" name="reference_height_mm" form="waterReferenceForm" type="number" min="230" max="5000" step="0.1" placeholder="Measure this height; do not guess" required>
          <button id="applyWaterReference" type="submit" form="waterReferenceForm">Apply zero-water reference</button>
          <p id="waterReferenceMessage" role="status" aria-live="polite"></p>
          <p class="note">Shared by all viewers. Stored in RAM only: recalibrate after restart or moving the probe. Does not change a saved record.</p>
        </details>
      </section>
      <h2 class="weather-subtitle">Online Weather Data</h2>
      <div id="weatherStatus" class="weather-status">
        Select a district and upazila to load automatic weather information.
      </div>

      <button class="secondary" type="button" id="refreshWeather">
        Get / Refresh Weather Information
      </button>

      <div id="weatherPanel" hidden>
        <div class="weather-grid">
          <div class="weather-value"><span>Current model condition</span><strong id="weatherCondition">-</strong></div>
          <div class="weather-value"><span>Model temperature</span><strong id="weatherTemperature">-</strong></div>
          <div class="weather-value"><span>Model relative humidity</span><strong id="weatherHumidity">-</strong></div>
          <div class="weather-value"><span>Model cloud cover</span><strong id="weatherCloudCover">-</strong></div>
          <div class="weather-value"><span>Model UV index</span><strong id="weatherUvIndex">-</strong></div>
          <div class="weather-value"><span>Previous 7 completed-day rainfall</span><strong id="weatherRainfallTotal">-</strong></div>
        </div>

        <p class="history-title">Historical Rainfall, UV and Temperature &mdash; Previous 7 Completed Days</p>
        <div class="history-wrap">
          <table>
            <thead><tr><th>Date (dd/mm/yyyy)</th><th>Rainfall estimate</th><th>Rain hours</th><th>UV Max</th><th>Temperature</th></tr></thead>
            <tbody id="weatherHistory"></tbody>
          </table>
        </div>
        <p class="note">Historical rows intentionally do not show daily weather-condition codes. They are observation-initialized model estimates, not field or station observations.</p>

        <section id="hourlyChartPanel" class="hourly-chart" hidden>
          <div class="chart-heading">
            <p class="history-title">Hourly Rainfall and Temperature &mdash; Previous 7 Completed Days</p>
            <label class="chart-toggle"><input type="checkbox" id="humidityToggle" aria-controls="hourlyChart humidityLegend uvLegend"> Show humidity &amp; UV index</label>
          </div>
          <div id="hourlyChartStatus" class="weather-status loading">Loading hourly weather graph...</div>
          <div id="chartWrap" class="chart-wrap">
            <canvas id="hourlyChart" aria-label="Hourly rainfall and temperature graph"></canvas>
            <div id="hourlyChartTooltip" class="chart-tooltip" hidden></div>
          </div>
          <div class="chart-legend" aria-label="Graph legend">
            <span class="legend-item"><i class="legend-swatch legend-rain"></i>Rainfall</span>
            <span class="legend-item"><i class="legend-swatch legend-temperature"></i>Temperature</span>
            <span class="legend-item" id="humidityLegend" hidden><i class="legend-swatch legend-humidity"></i>Humidity</span>
            <span class="legend-item" id="uvLegend" hidden><i class="legend-swatch legend-uv"></i>UV index</span>
          </div>
          <p id="hourlyChartUvNote" class="chart-extra-note" hidden></p>
          <p class="note" id="hourlyChartMeta"></p>
        </section>

        <p class="note" id="weatherMeta"></p>
      </div>

      <div class="section-title">Field Information</div>

      <label for="block">Block</label>
      <input type="text" id="block" name="block" maxlength="31"
             placeholder="Example: Block A" required>

      <label for="plotId">Plot ID</label>
      <input type="text" id="plotId" name="plot_id" maxlength="39"
             placeholder="Example: GZ-01-P05" required>

      <label for="plotArea">Plot Area (m&sup2;)</label>
      <input type="number" id="plotArea" name="plot_area_m2" min="0" max="10000000"
             step="any" inputmode="decimal" placeholder="Example: 1250.5"
             required>

      <label for="crop">Crop</label>
      <input type="text" id="crop" name="crop" maxlength="47"
             placeholder="Example: Rice" required>

      <label for="variety">Variety</label>
      <input type="text" id="variety" name="variety" maxlength="47"
             placeholder="Example: BRRI dhan89" required>

      <label for="soilType">Soil Type</label>
      <select id="soilType" name="soil_type" aria-describedby="soilTypeHelp" required>
        <option value="" selected disabled>Select soil type</option>
        <option value="Sand">Sand</option>
        <option value="Loamy sand">Loamy sand</option>
        <option value="Sandy loam">Sandy loam</option>
        <option value="Loam">Loam</option>
        <option value="Silt loam">Silt loam</option>
        <option value="Silt">Silt</option>
        <option value="Sandy clay loam">Sandy clay loam</option>
        <option value="Clay loam">Clay loam</option>
        <option value="Silty clay loam">Silty clay loam</option>
        <option value="Sandy clay">Sandy clay</option>
        <option value="Silty clay">Silty clay</option>
        <option value="Clay">Clay</option>
        <option value="Unknown">Unknown</option>
      </select>
      <p class="note" id="soilTypeHelp">USDA soil-texture classes. Choose Unknown if unconfirmed; future advice will treat it as missing soil information.</p>

      <label for="water">Irrigation Water Level (mm)</label>
      <input type="number" id="water" name="water" min="0" max="10000"
             step="0.1" placeholder="Example: 25.5" required>
      <input type="hidden" id="waterSource" name="water_source" value="manual">
      <button id="useWaterReading" class="secondary" type="button" disabled>Use current JSN water level</button>
      <p id="waterSourceNote" class="note">Manual entry is available. Selecting JSN uses a fresh calibrated sensor reading when saving.</p>

      <label for="tensiometer">Tensiometer Reading (cb)</label>
      <input type="number" id="tensiometer" name="tensiometer" min="0" max="1000"
             step="0.1" placeholder="Example: 35.0" required>

      <label>Current Irrigation Pump State</label>
      <div class="pump-options">
        <label class="pump-option"><input type="radio" name="pump" value="ON" required>ON</label>
        <label class="pump-option"><input type="radio" name="pump" value="OFF">OFF</label>
      </div>

      <section class="local-sensor-panel" aria-labelledby="advisoryTitle">
        <h2 id="advisoryTitle" class="weather-subtitle">Irrigation Advisory &mdash; Groq</h2>
        <p class="note">Prototype data review only. Crop stage, validated local thresholds, irrigation method, pump flow and efficiency are not configured. Start / stop decisions and watering minutes are therefore unavailable.</p>
        <p class="note">Clicking below sends these field entries, selected location, available model weather and local sensor readings to Groq. Local sensors describe the device position, which is not GPS-verified against the selected plot. The key is never sent to your browser.</p>
        <button id="getAdvisory" class="secondary" type="button">Get Groq Field Review</button>
        <p id="advisoryStatus" class="weather-status" role="status" aria-live="polite">Complete the field information to request a review.</p>
        <p id="advisoryExplanation" style="white-space:pre-wrap"></p>
        <ul id="advisoryLimitations"></ul>
        <p id="advisoryMeta" class="note"></p>
        <p class="note">AI explanations can be wrong. Verify them with an agricultural adviser. This feature never operates a pump and does not save a field record.</p>
      </section>
      <button id="saveButton" type="submit" disabled>Save Field Information</button>
    </form>
    <!-- Separate form ownership keeps calibration validation out of field entry. -->
    <form id="sensorReferenceForm"></form>
    <form id="waterReferenceForm"></form>

    <a class="download" href="/download">Download Saved CSV Log</a>
    <p class="note">Location is checked against the built-in 503-Upazila Bangladesh table. Current weather is a provisional grid-model estimate; history uses Open-Meteo Historical Forecast data when available. Neither is a farm or BMD station observation. The latest 100 saved records remain available while the ESP32 is powered on.</p>
  </div>

  <script>
    const districtInput = document.getElementById('district');
    const upazilaInput = document.getElementById('upazila');
    const districtList = document.getElementById('districts');
    const upazilaList = document.getElementById('upazilas');
    const weatherStatus = document.getElementById('weatherStatus');
    const weatherPanel = document.getElementById('weatherPanel');
    const historyBody = document.getElementById('weatherHistory');
    const saveButton = document.getElementById('saveButton');
    const refreshWeatherButton = document.getElementById('refreshWeather');
    const hourlyChartPanel = document.getElementById('hourlyChartPanel');
    const hourlyChartStatus = document.getElementById('hourlyChartStatus');
    const hourlyChartCanvas = document.getElementById('hourlyChart');
    const hourlyChartTooltip = document.getElementById('hourlyChartTooltip');
    const hourlyChartMeta = document.getElementById('hourlyChartMeta');
    const chartWrap = document.getElementById('chartWrap');
    const humidityToggle = document.getElementById('humidityToggle');
    const humidityLegend = document.getElementById('humidityLegend');
    const uvLegend = document.getElementById('uvLegend');
    const hourlyChartUvNote = document.getElementById('hourlyChartUvNote');
    let weatherReady = false;
    let weatherRequestId = 0;
    let weatherRequestController = null;
    let hourlyChartData = null;
    let hourlyChartRequestId = 0;
    let hourlyChartRequestController = null;
    let selectedHourlyChartIndex = -1;
    let hourlyChartDrawQueued = false;
    let activeWeatherTransfers = 0;

    function changeWeatherTransferCount(delta) {
      activeWeatherTransfers = Math.max(0, activeWeatherTransfers + delta);
      window.dispatchEvent(new CustomEvent('efb-weather-busy', { detail: activeWeatherTransfers }));
    }

    function setStatus(message, type) {
      weatherStatus.textContent = message;
      weatherStatus.className = 'weather-status' + (type ? ' ' + type : '');
    }

    function setHourlyChartStatus(message, type) {
      hourlyChartStatus.textContent = message;
      hourlyChartStatus.className = 'weather-status' + (type ? ' ' + type : '');
    }

    function invalidateWeather() {
      weatherRequestId++;
      hourlyChartRequestId++;
      if (weatherRequestController) weatherRequestController.abort();
      if (hourlyChartRequestController) hourlyChartRequestController.abort();
      weatherRequestController = null;
      hourlyChartRequestController = null;
      refreshWeatherButton.disabled = false;
      hourlyChartData = null;
      selectedHourlyChartIndex = -1;
      hourlyChartPanel.hidden = true;
      chartWrap.hidden = true;
      hourlyChartTooltip.hidden = true;
      hourlyChartMeta.textContent = '';
      humidityToggle.checked = false;
      humidityToggle.disabled = false;
      updateHourlyChartOptions();
      weatherReady = false;
      saveButton.disabled = true;
      weatherPanel.hidden = true;
      setStatus('Location changed. Refresh weather information before saving.', '');
    }

    function setOptions(list, values) {
      list.replaceChildren();
      values.forEach(function(optionValue) {
        const option = document.createElement('option');
        option.value = optionValue;
        list.appendChild(option);
      });
    }

    async function getJson(url, timeoutMs, controller) {
      const blocksLocalUpdates = url.startsWith('/weather?') || url.startsWith('/weather-chart?');
      if (blocksLocalUpdates) changeWeatherTransferCount(1);
      const requestController = controller || new AbortController();
      const timeout = window.setTimeout(function() {
        requestController.abort();
      }, timeoutMs || 10000);

      try {
        const response = await fetch(url, {
          cache: 'no-store',
          signal: requestController.signal
        });
        const data = await response.json();
        if (!response.ok || !data.ok) {
          throw new Error(data.error || 'The requested information could not be loaded.');
        }
        return data;
      } catch (error) {
        if (requestController.signal.aborted) {
          throw new Error('Request timed out. Please try again.');
        }
        throw error;
      } finally {
        window.clearTimeout(timeout);
        if (blocksLocalUpdates) changeWeatherTransferCount(-1);
      }
    }

    async function loadDistricts() {
      try {
        const data = await getJson('/api/districts');
        setOptions(districtList, data.districts);
      } catch (error) {
        setStatus(error.message || 'The district list could not be loaded.', 'error');
      }
    }

    async function loadUpazilas() {
      const district = districtInput.value.trim();
      setOptions(upazilaList, []);
      upazilaInput.value = '';

      if (!district) return;

      try {
        const query = new URLSearchParams({ district: district });
        const data = await getJson('/api/upazilas?' + query.toString());
        districtInput.value = data.district;
        setOptions(upazilaList, data.upazilas);
        setStatus('Select an Upazila from the verified built-in list.', '');
      } catch (error) {
        setStatus(error.message || 'No Upazilas were found for this District.', 'error');
      }
    }

    function value(number, digits, suffix) {
      return Number(number).toFixed(digits) + suffix;
    }

    function formatRegionalDate(isoDate) {
      if (typeof isoDate !== 'string' || isoDate.length < 10 ||
          isoDate.charAt(4) !== '-' || isoDate.charAt(7) !== '-') {
        return isoDate || '-';
      }
      return isoDate.slice(8, 10) + '/' + isoDate.slice(5, 7) + '/' + isoDate.slice(0, 4);
    }

    function formatRegionalDateTime(isoDateTime) {
      const date = formatRegionalDate(isoDateTime);
      if (typeof isoDateTime !== 'string' || isoDateTime.length <= 10) return date;
      const time = isoDateTime.slice(11);
      return time ? date + ' ' + time : date;
    }

    function formatRegionalDateObject(date, includeYear) {
      const day = String(date.getUTCDate()).padStart(2, '0');
      const month = String(date.getUTCMonth() + 1).padStart(2, '0');
      return includeYear ? day + '/' + month + '/' + date.getUTCFullYear() : day + '/' + month;
    }

    function renderWeather(data) {
      document.getElementById('weatherCondition').textContent = data.current_weather_summary;
      document.getElementById('weatherTemperature').textContent = value(data.temperature_c, 1, ' \u00B0C');
      document.getElementById('weatherHumidity').textContent = value(data.relative_humidity_percent, 0, ' %');
      document.getElementById('weatherCloudCover').textContent = value(data.cloud_cover_percent, 0, ' %');
      document.getElementById('weatherUvIndex').textContent = value(data.current_uv_index, 1, '');
      document.getElementById('weatherRainfallTotal').textContent = value(data.previous_7_completed_days_rainfall_mm, 1, ' mm');

      historyBody.replaceChildren();
      data.history.forEach(function(day) {
        const row = document.createElement('tr');
        [
          formatRegionalDate(day.date),
          value(day.rainfall_mm, 1, ' mm'),
          value(day.precipitation_hours, 1, ' h'),
          value(day.uv_index_max, 1, ''),
          value(day.temperature_min_c, 1, ' to ') + value(day.temperature_max_c, 1, ' \u00B0C')
        ].forEach(function(cellValue) {
          const cell = document.createElement('td');
          cell.textContent = cellValue;
          row.appendChild(cell);
        });
        historyBody.appendChild(row);
      });

      document.getElementById('weatherMeta').textContent =
        'Weather time: ' + formatRegionalDateTime(data.weather_time) +
        ' | Coordinates: ' + Number(data.latitude).toFixed(4) + ', ' + Number(data.longitude).toFixed(4) +
        ' | Location reference: ' + data.location_reference +
        ' | Table: ' + data.location_dataset_version +
        ' | Current: ' + data.current_data_type +
        ' | History: ' + data.history_data_type +
        ' | Current grid: ' + Number(data.forecast_grid_latitude).toFixed(4) + ', ' + Number(data.forecast_grid_longitude).toFixed(4) +
        ' | History grid: ' + Number(data.history_grid_latitude).toFixed(4) + ', ' + Number(data.history_grid_longitude).toFixed(4);
      weatherPanel.hidden = false;
    }

    function chartLayout(width, height, showExtraSeries) {
      const left = 42;
      const right = 45;
      const top = 18;
      const bottom = height - 28;
      const mainBottom = showExtraSeries ? Math.max(top + 92, Math.round(height * 0.56)) : bottom;
      const humidityTop = mainBottom + 38;
      return {
        left: left,
        right: width - right,
        top: top,
        mainBottom: mainBottom,
        humidityTop: humidityTop,
        humidityBottom: bottom,
        width: Math.max(1, width - left - right)
      };
    }

    function chartX(layout, index, count) {
      return layout.left + (index * layout.width / Math.max(1, count - 1));
    }

    function chartY(value, minimum, maximum, top, bottom) {
      return bottom - ((value - minimum) / Math.max(0.001, maximum - minimum)) * (bottom - top);
    }

    function chartRange(values) {
      const finite = values.filter(function(item) { return Number.isFinite(item); });
      let minimum = Math.min.apply(null, finite);
      let maximum = Math.max.apply(null, finite);
      if (!Number.isFinite(minimum) || !Number.isFinite(maximum)) return { minimum: 0, maximum: 1 };
      if (minimum === maximum) {
        minimum -= 1;
        maximum += 1;
      }
      return { minimum: Math.floor(minimum - 1), maximum: Math.ceil(maximum + 1) };
    }

    function hourlyDate(index) {
      const date = new Date(hourlyChartData.startDate + 'T00:00:00Z');
      date.setUTCDate(date.getUTCDate() + Math.floor(index / 24));
      return date;
    }

    function hourlyDayLabel(index) {
      return formatRegionalDateObject(hourlyDate(index), false);
    }

    function hourlyDetailText(index) {
      const hour = String(index % 24).padStart(2, '0') + ':00';
      const rain = hourlyChartData.rainfall[index];
      const temperature = hourlyChartData.temperature[index];
      const humidity = hourlyChartData.humidity[index];
      const uv = hourlyChartData.uv[index];
      let text = formatRegionalDateObject(hourlyDate(index), true) + ' ' + hour + ' Bangladesh time\n' +
        'Rainfall: ' + (Number.isFinite(rain) ? rain.toFixed(1) + ' mm' : 'not available') + '\n' +
        'Temperature: ' + (Number.isFinite(temperature) ? temperature.toFixed(1) + ' \u00B0C' : 'not available');
      if (humidityToggle.checked) {
        text += '\nHumidity: ' + (Number.isFinite(humidity) ? humidity.toFixed(0) + ' %' : 'not available') +
          '\nUV index: ' + (Number.isFinite(uv) ? uv.toFixed(1) : 'not available');
      }
      return text;
    }

    function updateHourlyChartTooltip(layout) {
      if (!hourlyChartData || selectedHourlyChartIndex < 0) {
        hourlyChartTooltip.hidden = true;
        return;
      }

      const width = hourlyChartCanvas.clientWidth;
      const x = chartX(layout, selectedHourlyChartIndex, hourlyChartData.rainfall.length);
      hourlyChartTooltip.textContent = hourlyDetailText(selectedHourlyChartIndex);
      hourlyChartTooltip.hidden = false;
      hourlyChartTooltip.style.left = Math.max(5, Math.min(x + 8, width - 210)) + 'px';
      hourlyChartTooltip.style.top = '8px';
    }

    function drawSeries(context, values, color, minimum, maximum, top, bottom, layout) {
      context.beginPath();
      let hasPoint = false;
      let hasAnyPoint = false;
      values.forEach(function(valueAtIndex, index) {
        if (!Number.isFinite(valueAtIndex)) { hasPoint = false; return; }
        const x = chartX(layout, index, values.length);
        const y = chartY(valueAtIndex, minimum, maximum, top, bottom);
        if (hasPoint) context.lineTo(x, y);
        else {
          context.moveTo(x, y);
          hasPoint = true;
        }
        hasAnyPoint = true;
      });
      if (hasAnyPoint) {
        context.strokeStyle = color;
        context.lineWidth = 1.7;
        context.stroke();
      }
    }

    function drawHourlyChart() {
      if (!hourlyChartData) return;
      const context = hourlyChartCanvas.getContext('2d');
      const width = hourlyChartCanvas.clientWidth;
      const height = hourlyChartCanvas.clientHeight;
      if (!context || width < 140 || height < 160) return;

      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const targetWidth = Math.round(width * dpr);
      const targetHeight = Math.round(height * dpr);
      if (hourlyChartCanvas.width !== targetWidth || hourlyChartCanvas.height !== targetHeight) {
        hourlyChartCanvas.width = targetWidth;
        hourlyChartCanvas.height = targetHeight;
      }
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.clearRect(0, 0, width, height);

      const showHumidity = humidityToggle.checked;
      const layout = chartLayout(width, height, showHumidity);
      const rainMaximum = Math.max(1, Math.ceil(Math.max.apply(null, hourlyChartData.rainfall.filter(Number.isFinite)) * 2) / 2);
      const temperatureRange = chartRange(hourlyChartData.temperature);
      const uvMaximum = Math.max(2, Math.ceil(Math.max(0, ...hourlyChartData.uv.filter(Number.isFinite)) / 2) * 2);

      context.font = '10px Arial';
      context.textBaseline = 'middle';
      context.textAlign = 'left';
      context.fillStyle = '#1873a2';
      context.fillText('Rain (mm)', layout.left, 9);
      context.textAlign = 'right';
      context.fillStyle = '#6548c9';
      context.fillText('Temp. (\u00B0C)', layout.right, 9);
      for (let tick = 0; tick <= 4; tick++) {
        const fraction = tick / 4;
        const y = layout.top + fraction * (layout.mainBottom - layout.top);
        const rainValue = rainMaximum * (1 - fraction);
        const temperatureValue = temperatureRange.maximum - fraction * (temperatureRange.maximum - temperatureRange.minimum);
        context.strokeStyle = '#d8e4da';
        context.lineWidth = 1;
        context.beginPath();
        context.moveTo(layout.left, y);
        context.lineTo(layout.right, y);
        context.stroke();
        context.fillStyle = '#607060';
        context.textAlign = 'right';
        context.fillText(rainValue.toFixed(rainMaximum < 2 ? 1 : 0), layout.left - 5, y);
        context.textAlign = 'left';
        context.fillText(temperatureValue.toFixed(0) + '\u00B0', layout.right + 5, y);
      }

      const barWidth = Math.max(1, layout.width / Math.max(1, hourlyChartData.rainfall.length - 1) * 0.72);
      context.fillStyle = '#1b9eea';
      hourlyChartData.rainfall.forEach(function(rainValue, index) {
        if (!Number.isFinite(rainValue) || rainValue <= 0) return;
        const x = chartX(layout, index, hourlyChartData.rainfall.length);
        const y = chartY(rainValue, 0, rainMaximum, layout.top, layout.mainBottom);
        context.fillRect(x - barWidth / 2, y, barWidth, layout.mainBottom - y);
      });

      drawSeries(context, hourlyChartData.temperature, '#6548c9', temperatureRange.minimum,
                 temperatureRange.maximum, layout.top, layout.mainBottom, layout);

      for (let day = 0; day < 7; day++) {
        const firstIndex = day * 24;
        const lastIndex = Math.min(firstIndex + 23, hourlyChartData.rainfall.length - 1);
        const boundaryX = chartX(layout, firstIndex, hourlyChartData.rainfall.length);
        context.strokeStyle = '#c4d4c7';
        context.setLineDash([2, 3]);
        context.beginPath();
        context.moveTo(boundaryX, layout.top);
        context.lineTo(boundaryX, showHumidity ? layout.humidityBottom : layout.mainBottom);
        context.stroke();
        context.setLineDash([]);
        context.fillStyle = '#526856';
        context.textAlign = 'center';
        context.textBaseline = 'alphabetic';
        context.fillText(hourlyDayLabel(firstIndex), (chartX(layout, firstIndex, hourlyChartData.rainfall.length) + chartX(layout, lastIndex, hourlyChartData.rainfall.length)) / 2, height - 8);
      }

      if (showHumidity) {
        context.fillStyle = '#607060';
        context.textAlign = 'left';
        context.textBaseline = 'alphabetic';
        context.fillText('Humidity', layout.left, layout.humidityTop - 10);
        context.fillStyle = '#b76a00';
        context.textAlign = 'right';
        context.fillText('UV index', layout.right, layout.humidityTop - 10);
        [100, 50, 0].forEach(function(humidityValue) {
          const y = chartY(humidityValue, 0, 100, layout.humidityTop, layout.humidityBottom);
          context.strokeStyle = '#d8e4da';
          context.beginPath();
          context.moveTo(layout.left, y);
          context.lineTo(layout.right, y);
          context.stroke();
          context.fillStyle = '#607060';
          context.textAlign = 'right';
          context.textBaseline = 'middle';
          context.fillText(humidityValue + '%', layout.left - 5, y);
          context.textAlign = 'left';
          context.fillStyle = '#b76a00';
          context.fillText((humidityValue / 100 * uvMaximum).toFixed(0), layout.right + 5, y);
        });
        drawSeries(context, hourlyChartData.humidity, '#149c68', 0, 100,
                   layout.humidityTop, layout.humidityBottom, layout);
        drawSeries(context, hourlyChartData.uv, '#b76a00', 0, uvMaximum,
                   layout.humidityTop, layout.humidityBottom, layout);
      }

      if (selectedHourlyChartIndex >= 0 && selectedHourlyChartIndex < hourlyChartData.rainfall.length) {
        const x = chartX(layout, selectedHourlyChartIndex, hourlyChartData.rainfall.length);
        context.strokeStyle = '#193b24';
        context.lineWidth = 1;
        context.beginPath();
        context.moveTo(x, layout.top);
        context.lineTo(x, showHumidity ? layout.humidityBottom : layout.mainBottom);
        context.stroke();

        const temperature = hourlyChartData.temperature[selectedHourlyChartIndex];
        if (Number.isFinite(temperature)) {
          context.fillStyle = '#6548c9';
          context.beginPath();
          context.arc(x, chartY(temperature, temperatureRange.minimum, temperatureRange.maximum, layout.top, layout.mainBottom), 3.2, 0, Math.PI * 2);
          context.fill();
        }
        const rain = hourlyChartData.rainfall[selectedHourlyChartIndex];
        if (Number.isFinite(rain) && rain > 0) {
          context.fillStyle = '#1b9eea';
          context.beginPath();
          context.arc(x, chartY(rain, 0, rainMaximum, layout.top, layout.mainBottom), 3, 0, Math.PI * 2);
          context.fill();
        }
        if (showHumidity) {
          const humidity = hourlyChartData.humidity[selectedHourlyChartIndex];
          if (Number.isFinite(humidity)) {
            context.fillStyle = '#149c68';
            context.beginPath();
            context.arc(x, chartY(humidity, 0, 100, layout.humidityTop, layout.humidityBottom), 3.2, 0, Math.PI * 2);
            context.fill();
          }
          const uv = hourlyChartData.uv[selectedHourlyChartIndex];
          if (Number.isFinite(uv)) {
            context.fillStyle = '#b76a00';
            context.beginPath();
            context.arc(x, chartY(uv, 0, uvMaximum, layout.humidityTop, layout.humidityBottom), 3.2, 0, Math.PI * 2);
            context.fill();
          }
        }
        updateHourlyChartTooltip(layout);
      } else {
        hourlyChartTooltip.hidden = true;
      }
    }

    function queueHourlyChartDraw() {
      if (!hourlyChartData || hourlyChartDrawQueued) return;
      hourlyChartDrawQueued = true;
      window.requestAnimationFrame(function() {
        hourlyChartDrawQueued = false;
        drawHourlyChart();
      });
    }

    function decodeTenths(values) {
      return values.map(function(item) {
        return item === null ? NaN : Number(item) / 10;
      });
    }

    function decodePercent(values) {
      return values.map(function(item) {
        return item === null ? NaN : Number(item);
      });
    }

    function decodeUv(values, sampleCount) {
      if (!Array.isArray(values) || values.length !== sampleCount) return Array(sampleCount).fill(NaN);
      return values.map(item => Number.isInteger(item) && item >= 0 && item <= 32767 ? item / 10 : NaN);
    }

    function updateHourlyChartOptions() {
      const show = humidityToggle.checked && hourlyChartData !== null;
      humidityLegend.hidden = !show;
      uvLegend.hidden = !show;
      chartWrap.classList.toggle('show-extra-series', show);
      hourlyChartCanvas.setAttribute('aria-label', show ?
        'Hourly rainfall, temperature, relative humidity and UV index graph' :
        'Hourly rainfall and temperature graph');
      const missingUv = hourlyChartData ? hourlyChartData.uv.filter(item => !Number.isFinite(item)).length : 0;
      hourlyChartUvNote.textContent = missingUv === 168 ?
        'Hourly UV index is unavailable from this source. Rainfall, temperature and humidity remain available.' :
        missingUv > 0 ? 'UV index is unavailable for ' + missingUv + ' hours; gaps are not zero UV.' :
        'Lower chart: humidity (%) on the left; UV index (unitless) on the right.';
      hourlyChartUvNote.hidden = !show;
    }

    async function loadHourlyChart(district, upazila) {
      const requestId = ++hourlyChartRequestId;
      if (hourlyChartRequestController) hourlyChartRequestController.abort();
      const requestController = new AbortController();
      hourlyChartRequestController = requestController;
      hourlyChartPanel.hidden = false;
      chartWrap.hidden = true;
      humidityToggle.disabled = true;
      setHourlyChartStatus('Loading hourly weather graph...', 'loading');
      try {
        const query = new URLSearchParams({ district: district, upazila: upazila });
        const data = await getJson('/weather-chart?' + query.toString(), 5000, requestController);
        if (requestId !== hourlyChartRequestId) return;
        const sampleCount = Number(data.sample_count);
        if (sampleCount !== 168 || !Array.isArray(data.rainfall_tenths_mm) ||
            !Array.isArray(data.temperature_tenths_c) || !Array.isArray(data.relative_humidity_percent) ||
            data.rainfall_tenths_mm.length !== sampleCount ||
            data.temperature_tenths_c.length !== sampleCount ||
            data.relative_humidity_percent.length !== sampleCount) {
          throw new Error('Hourly graph data were incomplete.');
        }

        hourlyChartData = {
          startDate: data.start_date,
          rainfall: decodeTenths(data.rainfall_tenths_mm),
          temperature: decodeTenths(data.temperature_tenths_c),
          humidity: decodePercent(data.relative_humidity_percent),
          uv: decodeUv(data.uv_index_tenths, sampleCount)
        };
        selectedHourlyChartIndex = -1;
        humidityToggle.disabled = false;
        updateHourlyChartOptions();
        hourlyChartMeta.textContent = 'Bangladesh time | Data fetched: ' + data.chart_fetched_at +
          ' | Grid: ' + Number(data.grid_latitude).toFixed(4) + ', ' + Number(data.grid_longitude).toFixed(4) +
          ' | Source: ' + data.history_source + ' | ' + data.history_data_type;
        chartWrap.hidden = false;
        if (data.history_source === 'Open-Meteo Historical Forecast API') {
          setHourlyChartStatus('Hourly graph loaded. Tap a time to view its values.', '');
        } else {
          setHourlyChartStatus('Hourly graph is using a clearly labelled forecast fallback.', 'loading');
        }
        queueHourlyChartDraw();
      } catch (error) {
        if (requestId !== hourlyChartRequestId) return;
        hourlyChartData = null;
        humidityToggle.disabled = true;
        updateHourlyChartOptions();
        chartWrap.hidden = true;
        hourlyChartTooltip.hidden = true;
        hourlyChartMeta.textContent = '';
        setHourlyChartStatus('Hourly chart unavailable; field records can still be saved. ' + (error.message || ''), 'error');
      } finally {
        if (hourlyChartRequestController === requestController) {
          hourlyChartRequestController = null;
        }
      }
    }

    async function loadWeather() {
      const district = districtInput.value.trim();
      const upazila = upazilaInput.value.trim();

      if (!district || !upazila) {
        weatherReady = false;
        saveButton.disabled = true;
        setStatus('Enter both District and Upazila first.', 'error');
        return;
      }

      const requestId = ++weatherRequestId;
      if (weatherRequestController) weatherRequestController.abort();
      if (hourlyChartRequestController) hourlyChartRequestController.abort();
      hourlyChartRequestId++;
      const requestController = new AbortController();
      weatherRequestController = requestController;
      weatherReady = false;
      saveButton.disabled = true;
      weatherPanel.hidden = true;
      refreshWeatherButton.disabled = true;
      setStatus('Loading model weather information...', 'loading');

      try {
        const query = new URLSearchParams({ district: district, upazila: upazila });
        const data = await getJson('/weather?' + query.toString(), 25000, requestController);
        if (requestId !== weatherRequestId) return;

        renderWeather(data);
        districtInput.value = data.district;
        upazilaInput.value = data.upazila;
        weatherReady = true;
        saveButton.disabled = false;
        loadHourlyChart(data.district, data.upazila);
        if (data.history_source === 'Open-Meteo Historical Forecast API') {
          setStatus('Current model context and seven completed historical days loaded. You may now save the field record.', '');
        } else {
          setStatus('Historical Forecast data were unavailable. The history is clearly marked as a forecast fallback.', 'loading');
        }
      } catch (error) {
        if (requestId !== weatherRequestId) return;
        setStatus(error.message || 'Weather data could not be loaded.', 'error');
      } finally {
        if (weatherRequestController === requestController) {
          weatherRequestController = null;
        }
        if (requestId === weatherRequestId) {
          refreshWeatherButton.disabled = false;
        }
      }
    }

    districtInput.addEventListener('input', invalidateWeather);
    upazilaInput.addEventListener('input', invalidateWeather);
    districtInput.addEventListener('change', function() {
      invalidateWeather();
      loadUpazilas();
    });
    upazilaInput.addEventListener('change', function() {
      if (districtInput.value.trim() && upazilaInput.value.trim()) loadWeather();
    });
    refreshWeatherButton.addEventListener('click', loadWeather);

    humidityToggle.addEventListener('change', function() {
      updateHourlyChartOptions();
      queueHourlyChartDraw();
    });

    function selectHourlyChartPoint(event) {
      if (!hourlyChartData) return;
      const rectangle = hourlyChartCanvas.getBoundingClientRect();
      const layout = chartLayout(rectangle.width, rectangle.height, humidityToggle.checked);
      const x = event.clientX - rectangle.left;
      if (x < layout.left - 10 || x > layout.right + 10) return;
      selectedHourlyChartIndex = Math.max(0, Math.min(
        hourlyChartData.rainfall.length - 1,
        Math.round((x - layout.left) / layout.width * (hourlyChartData.rainfall.length - 1))
      ));
      queueHourlyChartDraw();
    }

    hourlyChartCanvas.addEventListener('pointerdown', selectHourlyChartPoint);
    hourlyChartCanvas.addEventListener('pointermove', function(event) {
      if (event.pointerType === 'mouse') selectHourlyChartPoint(event);
    });
    hourlyChartCanvas.addEventListener('pointerleave', function(event) {
      if (event.pointerType !== 'mouse') return;
      selectedHourlyChartIndex = -1;
      queueHourlyChartDraw();
    });

    if (window.ResizeObserver) {
      const hourlyChartResizeObserver = new ResizeObserver(queueHourlyChartDraw);
      hourlyChartResizeObserver.observe(hourlyChartCanvas);
    } else {
      window.addEventListener('resize', queueHourlyChartDraw);
    }

    loadDistricts();

    const plotAreaInput = document.getElementById('plotArea');
    function validatePlotArea() {
      const text = plotAreaInput.value.trim();
      const value = Number(text);
      const valid = /^(?:\d+(?:\.\d*)?|\.\d+)$/.test(text) &&
        Number.isFinite(value) && value > 0 && value <= 10000000;
      plotAreaInput.setCustomValidity(valid ? '' :
        'Enter a positive decimal plot area up to 10,000,000 square metres.');
      return valid;
    }
    plotAreaInput.addEventListener('input', validatePlotArea);
    plotAreaInput.addEventListener('change', validatePlotArea);

    document.getElementById('fieldForm').addEventListener('submit', function(event) {
      if (!validatePlotArea()) {
        event.preventDefault();
        plotAreaInput.reportValidity();
        return;
      }
      if (!weatherReady) {
        event.preventDefault();
        setStatus('Load weather information before saving this record.', 'error');
      }
    });
  </script>
  <script id="localSensorScript">
    (() => {
      'use strict';
      const element = id => document.getElementById(id);
      let busy = false;
      let inFlight = null;
      let lastSequence = null;
      let lastUptime = 0;
      let lastUvSequence = null;
      let lastUvUptime = 0;
      let uvSequenceChangedAt = 0;
      let sequenceChangedAt = 0;
      let lastResponseAt = 0;
      let hasResponse = false;
      let savingReference = false;
      let referenceDirty = false;
      let referenceRevision = 0;
      let referenceController = null;
      const referenceInput = element('sensorReferenceInput');

      function updateReferenceControls() {
        const disabled = busy || savingReference || !hasResponse;
        referenceInput.disabled = disabled;
        element('applySensorReference').disabled = disabled;
        element('resetSensorReference').disabled = disabled;
        element('applySensorReference').textContent = savingReference ? 'Applying...' : 'Apply reference';
      }
      function setReferenceMessage(text, kind = '') {
        const target = element('sensorReferenceMessage');
        target.textContent = text;
        target.className = text ? 'weather-status' + (kind ? ' ' + kind : '') : '';
      }

      function setLocalStatus(text, kind) {
        const target = element('localSensorStatus');
        if (target.textContent !== text) target.textContent = text;
        target.className = 'weather-status' + (kind ? ' ' + kind : '');
      }
      function clearLocalReadings(text, kind = 'error') {
        ['localSensorTemperature', 'localSensorHumidity', 'localSensorPressure', 'localSensorAltitude', 'localSensorGas'].forEach(id => {
          element(id).textContent = '\u2014';
        });
        element('localSensorMeta').textContent = 'No current reading displayed.';
        element('localSensorGasNote').textContent = 'Gas resistance is a raw sensor reading, not an IAQ score, CO2 concentration or safety alarm.';
        setLocalStatus(text, kind);
      }
      function showBusy() {
        clearLocalReadings('Display updates paused while online weather is loading. Sensor sampling continues.', 'loading');
        clearUvReading('LTR390 display paused while online weather is loading. Sampling continues.');
      }
      function clearUvReading(text) {
        element('localSensorUv').textContent = '\u2014';
        element('localSensorUvStatus').textContent = text;
      }
      function renderUvSensor(data, now) {
        if (typeof data.uv_sensor_ok !== 'boolean' || typeof data.uv_sensor_status !== 'string' ||
            !Number.isInteger(data.uv_sequence) || data.uv_sequence < 0 || data.uv_sequence > 4294967295) {
          clearUvReading('LTR390 unavailable: invalid UV sensor response.');
          return;
        }
        if (lastUvSequence === null || data.uv_sequence !== lastUvSequence || data.uptime_ms < lastUvUptime) {
          uvSequenceChangedAt = now;
        }
        lastUvSequence = data.uv_sequence;
        lastUvUptime = data.uptime_ms;
        if (!data.uv_sensor_ok) {
          clearUvReading(data.uv_sensor_status || 'LTR390 unavailable. BME680 readings are independent.');
          return;
        }
        if (!Number.isFinite(data.uv_index_estimate) || data.uv_index_estimate < 0 ||
            !Number.isInteger(data.uv_raw_counts) || data.uv_raw_counts < 0 || data.uv_raw_counts > 1048575 ||
            !Number.isFinite(data.uv_sample_age_ms) || data.uv_sample_age_ms < 0) {
          clearUvReading('LTR390 unavailable: invalid UV measurement.');
          return;
        }
        if (data.uv_sample_age_ms > 3500 || now - uvSequenceChangedAt >= 5000) {
          clearUvReading('LTR390 data are stale. Waiting for a fresh UV sample.');
          return;
        }
        element('localSensorUv').textContent = data.uv_index_estimate.toFixed(1);
        element('localSensorUvStatus').textContent = 'LTR390 live \u2014 UV estimate; sample age ' +
          (data.uv_sample_age_ms / 1000).toFixed(1) + ' s.';
      }
      function renderLocalSensor(data) {
        if (!data || data.ok !== true || !Number.isFinite(data.uptime_ms) ||
            data.uptime_ms < 0 ||
            !Number.isFinite(data.reference_pressure_hpa) ||
            data.reference_pressure_hpa < 800 || data.reference_pressure_hpa > 1100) {
          throw new Error('Invalid local sensor response.');
        }
        const now = performance.now();
        lastResponseAt = now;
        hasResponse = true;
        element('localSensorReference').textContent = data.reference_pressure_hpa.toFixed(2);
        const standardReference = Math.abs(data.reference_pressure_hpa - 1013.25) < 0.005;
        element('localSensorAltitudeLabel').textContent = standardReference ?
          'Pressure altitude (estimate)' : 'Barometric altitude (estimate)';
        element('localSensorReferenceMode').textContent = standardReference ?
          'Standard atmosphere reference; not necessarily your elevation above sea level.' :
          'Custom reference; update it as local weather changes.';
        if (!referenceDirty) referenceInput.value = data.reference_pressure_hpa.toFixed(2);
        updateReferenceControls();
        renderUvSensor(data, now);
        // Sensor-specific failures must not erase a healthy second sensor.
        if (typeof data.sensor_ok !== 'boolean' || typeof data.sensor_status !== 'string' ||
            !Number.isInteger(data.sequence) || data.sequence < 0) {
          clearLocalReadings('BME680 unavailable: invalid sensor response.');
          return;
        }
        if (lastSequence === null || data.sequence !== lastSequence || data.uptime_ms < lastUptime) {
          sequenceChangedAt = now;
        }
        lastSequence = data.sequence;
        lastUptime = data.uptime_ms;
        if (!data.sensor_ok) {
          clearLocalReadings(data.sensor_status || 'Local sensor unavailable. Online weather can still be used.');
          return;
        }
        if (!Number.isFinite(data.temperature_c) || !Number.isFinite(data.station_pressure_hpa) ||
            !Number.isFinite(data.relative_humidity_pct) ||
            !Number.isFinite(data.altitude_ft) || !Number.isFinite(data.sample_age_ms) ||
            data.sample_age_ms < 0 || data.temperature_c < -40 || data.temperature_c > 85 ||
            data.relative_humidity_pct < 0 || data.relative_humidity_pct > 100 ||
            data.station_pressure_hpa < 300 || data.station_pressure_hpa > 1100 ||
            typeof data.gas_available !== 'boolean' ||
            (data.gas_available && (!Number.isFinite(data.gas_resistance_ohms) ||
              data.gas_resistance_ohms <= 0))) {
          clearLocalReadings('BME680 unavailable: invalid sensor measurements.');
          return;
        }
        if (data.sample_age_ms > 3500 || now - sequenceChangedAt >= 5000) {
          clearLocalReadings('Local sensor data are stale. Waiting for a fresh sample.');
          return;
        }
        element('localSensorTemperature').textContent = data.temperature_c.toFixed(1);
        element('localSensorHumidity').textContent = data.relative_humidity_pct.toFixed(1);
        element('localSensorPressure').textContent = data.station_pressure_hpa.toFixed(2);
        element('localSensorAltitude').textContent = data.altitude_ft.toFixed(1);
        element('localSensorGas').textContent = data.gas_available ?
          (data.gas_resistance_ohms / 1000).toFixed(1) : '\u2014';
        element('localSensorGasNote').textContent = (data.gas_available ? '' : 'Gas reading unavailable; temperature, humidity and pressure are still live. ') +
          'Gas resistance is a raw sensor reading, not an IAQ score, CO2 concentration or safety alarm.';
        element('localSensorMeta').textContent = 'Received ' + new Date().toLocaleString('en-GB', {hour12:false}) +
          ' | sample age ' + (data.sample_age_ms / 1000).toFixed(1) + ' s';
        setLocalStatus('Live BME680 measurements \u2014 updates about once per second.', '');
      }
      async function pollLocalSensor() {
        let timeout;
        const revision = referenceRevision;
        try {
          if (busy) { showBusy(); return; }
          if (savingReference) return;
          const controller = new AbortController();
          inFlight = controller;
          timeout = window.setTimeout(() => controller.abort(), 3500);
          const response = await fetch('/api/local-sensor', {cache:'no-store', signal:controller.signal});
          const data = await response.json();
          if (!response.ok) throw new Error('Local sensor request failed.');
          if (!busy && !savingReference && revision === referenceRevision) renderLocalSensor(data);
        } catch (error) {
          if (revision !== referenceRevision) return;
          if (busy) showBusy();
          else if (savingReference) return;
          else {
            clearLocalReadings('Local readings temporarily unavailable. The ESP32 may be busy or disconnected; retrying...');
            clearUvReading('LTR390 unavailable: no valid response from the ESP32. Retrying...');
          }
        } finally {
          window.clearTimeout(timeout);
          inFlight = null;
          window.setTimeout(pollLocalSensor, 1000);
        }
      }
      async function applyReference(reset = false) {
        if (busy || savingReference || !hasResponse) return;
        if (!reset && !referenceInput.reportValidity()) return;
        const value = reset ? '1013.25' : referenceInput.value.trim();
        if (!value || !Number.isFinite(Number(value)) || Number(value) < 800 || Number(value) > 1100) {
          setReferenceMessage('Enter a reference from 800 to 1100 hPa.', 'error');
          return;
        }
        savingReference = true;
        ++referenceRevision; // Discard an older GET response arriving after this change.
        if (inFlight) inFlight.abort();
        updateReferenceControls();
        setReferenceMessage('Applying altitude reference...', 'loading');
        const controller = new AbortController();
        referenceController = controller;
        const timeout = window.setTimeout(() => controller.abort(), 6000);
        try {
          const response = await fetch('/api/local-sensor/reference', {
            method: 'POST', cache: 'no-store', signal: controller.signal,
            headers: {'Content-Type':'application/x-www-form-urlencoded', 'X-EFB-Request':'1'},
            body: new URLSearchParams({reference_pressure_hpa:Number(value).toFixed(2)}).toString()
          });
          const data = await response.json();
          if (!response.ok || data.ok !== true) {
            throw new Error(data.error || 'Reference update rejected by the ESP32.');
          }
          renderLocalSensor(data);
          referenceDirty = false;
          referenceInput.value = data.reference_pressure_hpa.toFixed(2);
          setReferenceMessage('Applied ' + data.reference_pressure_hpa.toFixed(2) +
            ' hPa. Altitude updated; measured pressure is unchanged.');
        } catch (error) {
          setReferenceMessage('Change not confirmed. Check the active reference before retrying. ' +
            (error.name === 'AbortError' ? 'Request timed out or weather loading started.' : error.message), 'error');
        } finally {
          window.clearTimeout(timeout);
          referenceController = null;
          savingReference = false;
          updateReferenceControls();
          if (busy) showBusy();
        }
      }
      referenceInput.addEventListener('input', () => {
        referenceDirty = true;
        setReferenceMessage('Not applied yet. Select Apply reference when ready.');
      });
      element('sensorReferenceForm').addEventListener('submit', event => {
        event.preventDefault();
        applyReference();
      });
      element('resetSensorReference').addEventListener('click', () => applyReference(true));
      window.addEventListener('efb-weather-busy', event => {
        busy = Number(event.detail) > 0;
        updateReferenceControls();
        if (busy) {
          if (inFlight) inFlight.abort();
          if (referenceController) referenceController.abort();
          showBusy();
        } else {
          setLocalStatus('Weather request finished. Waiting for a fresh local sample...', 'loading');
        }
      });
      window.setInterval(() => {
        if (!busy && hasResponse && performance.now() - lastResponseAt >= 5000) {
          clearLocalReadings('No recent response from the ESP32. Retrying local readings...');
          clearUvReading('LTR390 unavailable: no recent response from the ESP32. Retrying...');
        }
      }, 500);
      pollLocalSensor();
    })();
  </script>
  <script src="/field-extensions.js"></script>
</body>
</html>
)HTML";

// Convert the current model WMO code into a farmer-friendly Bangladesh label.
// Historical rows do not use this function because daily WMO codes represent
// only the most severe model category, not a day-long observed condition.
const char* currentWeatherSummaryBangladesh(int code) {
  switch (code) {
    case 0: return "Clear / sunny";
    case 1: return "Mostly sunny";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45:
    case 48: return "Fog or mist";
    case 51: return "Light drizzle";
    case 53: return "Drizzle";
    case 55: return "Dense drizzle";
    case 56:
    case 57: return "Drizzle / rain model signal";
    case 61: return "Slight rain";
    case 63: return "Moderate rain";
    case 65: return "Heavy rain";
    case 66:
    case 67: return "Rain model signal";
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86: return "Unusual cold-precipitation model signal";
    case 80: return "Slight rain showers";
    case 81: return "Rain showers";
    case 82: return "Heavy rain showers";
    case 95:
    case 96:
    case 99:
      return "Possible thunderstorm (model)";
    default: return "Unknown model category";
  }
}

const char* historySourceText(HistorySource source) {
  switch (source) {
    case HistorySource::HistoricalForecast:
      return "Open-Meteo Historical Forecast API";
    case HistorySource::ForecastFallback:
      return "Open-Meteo Forecast API fallback";
    default:
      return "Unknown history source";
  }
}

const char* historyDataTypeText(HistorySource source) {
  switch (source) {
    case HistorySource::HistoricalForecast:
      return "Observation-initialized grid-model estimate for completed days";
    case HistorySource::ForecastFallback:
      return "Forecast grid-model fallback; historical endpoint unavailable";
    default:
      return "Unknown history data type";
  }
}

void copyText(char* destination, size_t destinationSize, const char* source) {
  if (destinationSize == 0) return;
  if (source == nullptr) source = "";
  snprintf(destination, destinationSize, "%s", source);
}

void copyText(char* destination, size_t destinationSize, const String& source) {
  copyText(destination, destinationSize, source.c_str());
}

void setRecordTimestamp(char* destination, size_t destinationSize);
bool extractHourlyChart(JsonDocument& document,
                        const UpazilaCoordinate& location,
                        const char* startDate,
                        const char* endDate,
                        HistorySource source,
                        HourlyChartData& chart,
                        String& error);

// Make an English location key that ignores case, spaces and punctuation.
// A few common historical spelling/order variants are then mapped to the
// canonical spellings in the compiled Bangladesh table.
String locationKey(const char* source) {
  String result;
  if (source == nullptr) return result;

  result.reserve(strlen(source));
  for (size_t i = 0; source[i] != '\0'; i++) {
    char character = source[i];
    if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    if ((character >= 'a' && character <= 'z') ||
        (character >= '0' && character <= '9')) {
      result += character;
    }
  }

  if (result == "barisal") result = "barishal";
  else if (result == "chittagong") result = "chattogram";
  else if (result == "comilla") result = "cumilla";
  else if (result == "jessore") result = "jashore";
  else if (result == "bogra") result = "bogura";
  else if (result == "maulvibazar") result = "moulvibazar";
  else if (result == "nawabganj") result = "chapainawabganj";
  else if (result == "mokamtola") result = "mokamtala";
  else if (result == "bhulli") result = "bhully";
  else if (result == "bangara") result = "bangra";
  else if (result == "northfatikchhari" || result == "northfatikchari" ||
           result == "fatikcharinorth") result = "fatikchharinorth";
  else if (result == "southgafargaon" || result == "gafargaonsouth") result = "dakshingafargaon";

  return result;
}

const char* coordinateBasisText(CoordinateBasis basis) {
  switch (basis) {
    case CoordinateBasis::Bbs2022Adm3Interior:
      return "BBS 2022 Upazila boundary interior point";
    case CoordinateBasis::Gazette2026UnionAggregateInterior:
      return "2026 Gazette union-area interior point";
    case CoordinateBasis::Gazette2026ParentRemainderInterior:
      return "2026 adjusted parent-area interior point";
    case CoordinateBasis::Gazette2026PartialSplitSafeInterior:
      return "2026 Gazette safe interior point";
    default:
      return "Unknown coordinate basis";
  }
}

const char* coordinateBasisCsvText(CoordinateBasis basis) {
  switch (basis) {
    case CoordinateBasis::Bbs2022Adm3Interior:
      return "bbs_2022_adm3_interior";
    case CoordinateBasis::Gazette2026UnionAggregateInterior:
      return "gazette_2026_union_aggregate_interior";
    case CoordinateBasis::Gazette2026ParentRemainderInterior:
      return "gazette_2026_parent_remainder_interior";
    case CoordinateBasis::Gazette2026PartialSplitSafeInterior:
      return "gazette_2026_partial_split_safe_interior";
    default:
      return "unknown";
  }
}

int findUpazilaIndex(const char* district, const char* upazila) {
  String districtKey = locationKey(district);
  String upazilaKey = locationKey(upazila);

  if (districtKey.length() == 0 || upazilaKey.length() == 0) return -1;

  for (size_t index = 0; index < BUILTIN_UPAZILA_COUNT; index++) {
    const UpazilaCoordinate& entry = kBangladeshUpazilaCoordinates[index];
    if (districtKey == locationKey(entry.district) && upazilaKey == locationKey(entry.upazila)) {
      return static_cast<int>(index);
    }
  }

  return -1;
}

const char* findCanonicalDistrict(const char* district) {
  String districtKey = locationKey(district);
  if (districtKey.length() == 0) return nullptr;

  for (size_t index = 0; index < BUILTIN_UPAZILA_COUNT; index++) {
    const UpazilaCoordinate& entry = kBangladeshUpazilaCoordinates[index];
    if (districtKey == locationKey(entry.district)) return entry.district;
  }

  return nullptr;
}

bool parseFloatArg(const char* name, float minimum, float maximum, float& result) {
  String input = server.arg(name);
  input.trim();

  if (input.length() == 0) return false;

  const char* text = input.c_str();
  char* endPointer = nullptr;
  float value = strtof(text, &endPointer);

  if (endPointer == text || *endPointer != '\0' || !isfinite(value)) return false;
  if (value < minimum || value > maximum) return false;

  result = value;
  return true;
}

bool parsePumpState(bool& result) {
  String pump = server.arg("pump");

  if (pump == "ON") {
    result = true;
    return true;
  }
  if (pump == "OFF") {
    result = false;
    return true;
  }

  return false;
}

bool parseTextArg(const char* name,
                  const char* displayName,
                  char* destination,
                  size_t destinationSize,
                  String& error) {
  String value = server.arg(name);
  value.trim();

  if (value.length() == 0) {
    error = String(displayName) + " is required.";
    return false;
  }
  if (value.length() >= destinationSize) {
    error = String(displayName) + " is too long.";
    return false;
  }
  if (value.indexOf('\r') >= 0 || value.indexOf('\n') >= 0) {
    error = String("Invalid ") + displayName + " text.";
    return false;
  }

  copyText(destination, destinationSize, value);
  return true;
}

bool parsePlotAreaArg(double& result) {
  String input = server.arg("plot_area_m2");
  input.trim();
  if (input.length() == 0) return false;

  // Only plain decimal notation: reject hex, exponent notation, unit suffixes,
  // signs, non-finite spellings, embedded whitespace, and embedded NUL bytes.
  bool hasDigit = false;
  bool hasDecimalPoint = false;
  for (size_t i = 0; i < input.length(); ++i) {
    const char character = input[i];
    if (character >= '0' && character <= '9') {
      hasDigit = true;
    } else if (character == '.' && !hasDecimalPoint) {
      hasDecimalPoint = true;
    } else {
      return false;
    }
  }
  if (!hasDigit) return false;

  const char* text = input.c_str();
  char* endPointer = nullptr;
  const double value = strtod(text, &endPointer);
  if (endPointer == text || *endPointer != '\0' || !isfinite(value)) return false;
  if (value <= 0.0 || value > MAX_PLOT_AREA_M2) return false;
  result = value;
  return true;
}

bool parseSoilTypeArg(char* destination, size_t destinationSize) {
  String input = server.arg("soil_type");
  input.trim();
  for (const char* option : SOIL_TYPE_OPTIONS) {
    if (input == option) {
      copyText(destination, destinationSize, option);
      return true;
    }
  }
  return false;
}

// Shared by saving and future advice requests. This validates manual values
// only: no HTTP response, record insertion, weather fetch, or pump actuation.
bool parseManualFieldInputs(FieldData& record, String& error) {
  error = "";
  if (!parseTextArg("block", "Block", record.block, sizeof(record.block), error) ||
      !parseTextArg("plot_id", "Plot ID", record.plotId, sizeof(record.plotId), error)) {
    return false;
  }
  if (!parsePlotAreaArg(record.plotAreaM2)) {
    error = "Plot Area must be a positive decimal in square metres, up to 10000000.";
    return false;
  }
  if (!parseTextArg("crop", "Crop", record.crop, sizeof(record.crop), error) ||
      !parseTextArg("variety", "Variety", record.variety, sizeof(record.variety), error)) {
    return false;
  }
  if (!parseSoilTypeArg(record.soilType, sizeof(record.soilType))) {
    error = "Select a valid Soil Type, or Unknown if it is not confirmed.";
    return false;
  }
  if (!parseFloatArg("water", 0.0f, 10000.0f, record.irrigationWaterLevelMm)) {
    error = "Invalid irrigation water level.";
    return false;
  }
  if (!parseFloatArg("tensiometer", 0.0f, 1000.0f, record.tensiometerCb)) {
    error = "Invalid tensiometer reading.";
    return false;
  }
  if (!parsePumpState(record.pumpOn)) {
    error = "Invalid pump state.";
    return false;
  }
  return true;
}

bool requestJson(const String& url, JsonDocument& document, String& error) {
  // One outbound TLS transfer at a time. Requests are started only by this
  // WebServer task, so a weather transfer cannot overlap a newly queued review.
  if (IrrigationAdvisory::busy()) {
    error = "Groq review is running. Please refresh online weather after it finishes.";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    error = "ESP32 is not connected to the internet.";
    return false;
  }

  // Open-Meteo uses HTTPS. setInsecure() is used here for broad ESP32 core
  // compatibility; use a trusted root certificate for a production system.
  weatherHttp.end();
  weatherSecureClient.stop();
  weatherSecureClient.setInsecure();
  weatherSecureClient.setTimeout(WEATHER_HTTP_TIMEOUT_MS / 1000U);
  weatherHttp.setConnectTimeout(WEATHER_HTTP_TIMEOUT_MS);
  weatherHttp.setTimeout(WEATHER_HTTP_TIMEOUT_MS);

  if (!weatherHttp.begin(weatherSecureClient, url)) {
    error = "Could not start the weather request.";
    return false;
  }

  // Ask for uncompressed text. This avoids trying to parse a compressed HTTP
  // response as JSON on ESP32 boards.
  weatherHttp.addHeader("Accept-Encoding", "identity");
  weatherHttp.addHeader("User-Agent", "ESP32-Electronic-Field-Bag/1.0");

  int httpCode = weatherHttp.GET();
  if (httpCode != HTTP_CODE_OK) {
    error = String("Weather service returned HTTP ") + String(httpCode) + ".";
    weatherHttp.end();
    return false;
  }

  // Reading the completed payload first is more reliable than parsing the
  // live chunked HTTP stream on some ESP32/Arduino core combinations.
  String payload = weatherHttp.getString();
  weatherHttp.end();

  if (payload.length() == 0) {
    error = "Weather service returned an empty response.";
    return false;
  }

  DeserializationError jsonError = deserializeJson(document, payload);

  if (jsonError) {
    Serial.print("Weather JSON error: ");
    Serial.println(jsonError.c_str());
    Serial.print("Weather response starts with: ");
    Serial.println(payload.substring(0, 160));
    error = String("Could not read weather data: ") + jsonError.c_str();
    return false;
  }

  return true;
}

bool fetchCurrentAndFallbackHistory(const UpazilaCoordinate& location,
                                    WeatherData& weather,
                                    HourlyChartData& hourlyChart,
                                    bool& hourlyChartAvailable,
                                    String& error) {
  hourlyChartAvailable = false;
  const float latitude = location.latitudeE6 / 1000000.0f;
  const float longitude = location.longitudeE6 / 1000000.0f;
  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + String(latitude, 6) +
               "&longitude=" + String(longitude, 6) +
               "&current=temperature_2m,relative_humidity_2m,weather_code,cloud_cover,uv_index,precipitation" +
               "&daily=precipitation_sum,precipitation_hours,uv_index_max,temperature_2m_min,temperature_2m_max" +
               "&hourly=precipitation,temperature_2m,relative_humidity_2m,uv_index" +
               "&past_days=7&forecast_days=1&timezone=Asia%2FDhaka" +
               "&models=best_match&cell_selection=land";

  JsonDocument document;
  if (!requestJson(url, document, error)) return false;

  JsonObject current = document["current"].as<JsonObject>();
  JsonObject daily = document["daily"].as<JsonObject>();
  JsonArray dates = daily["time"].as<JsonArray>();
  JsonArray rainfall = daily["precipitation_sum"].as<JsonArray>();
  JsonArray precipitationHours = daily["precipitation_hours"].as<JsonArray>();
  JsonArray uvIndex = daily["uv_index_max"].as<JsonArray>();
  JsonArray temperatureMin = daily["temperature_2m_min"].as<JsonArray>();
  JsonArray temperatureMax = daily["temperature_2m_max"].as<JsonArray>();

  // With past_days=7 and forecast_days=1, indices 0..6 are the seven
  // completed days and the final row represents today.
  if (current.isNull() || daily.isNull() || dates.size() < HISTORY_DAYS + 1 ||
      rainfall.size() < HISTORY_DAYS + 1 || precipitationHours.size() < HISTORY_DAYS + 1 ||
      uvIndex.size() < HISTORY_DAYS + 1 || temperatureMin.size() < HISTORY_DAYS + 1 ||
      temperatureMax.size() < HISTORY_DAYS + 1) {
    error = "Weather service returned incomplete data.";
    return false;
  }

  if (current["temperature_2m"].isNull() || current["relative_humidity_2m"].isNull() ||
      current["weather_code"].isNull() || current["cloud_cover"].isNull() ||
      current["uv_index"].isNull() || current["precipitation"].isNull() ||
      current["time"].isNull()) {
    error = "Current model-weather data are incomplete.";
    return false;
  }

  memset(&weather, 0, sizeof(weather));
  weather.latitude = latitude;
  weather.longitude = longitude;
  weather.coordinateBasis = location.basis;
  weather.forecastGridLatitude = document["latitude"].isNull() ? latitude : document["latitude"].as<float>();
  weather.forecastGridLongitude = document["longitude"].isNull() ? longitude : document["longitude"].as<float>();
  weather.historyGridLatitude = weather.forecastGridLatitude;
  weather.historyGridLongitude = weather.forecastGridLongitude;
  weather.currentTemperatureC = current["temperature_2m"].as<float>();
  weather.relativeHumidityPercent = current["relative_humidity_2m"].as<float>();
  weather.cloudCoverPercent = current["cloud_cover"].as<float>();
  weather.currentPrecipitationMm = current["precipitation"].as<float>();
  weather.currentUvIndex = current["uv_index"].as<float>();
  weather.currentWeatherCode = current["weather_code"].as<int16_t>();
  copyText(weather.reportedAt, sizeof(weather.reportedAt), current["time"].as<const char*>());

  weather.previous7CompletedDaysRainfallMm = 0.0f;
  weather.historySource = HistorySource::ForecastFallback;
  for (uint8_t i = 0; i < HISTORY_DAYS; i++) {
    copyText(weather.history[i].date, sizeof(weather.history[i].date), dates[i].as<const char*>());
    weather.history[i].rainfallMm = rainfall[i].as<float>();
    weather.history[i].precipitationHours = precipitationHours[i].as<float>();
    weather.history[i].uvIndexMax = uvIndex[i].as<float>();
    weather.history[i].temperatureMinC = temperatureMin[i].as<float>();
    weather.history[i].temperatureMaxC = temperatureMax[i].as<float>();
    weather.previous7CompletedDaysRainfallMm += weather.history[i].rainfallMm;
  }

  String chartError;
  if (extractHourlyChart(document, location, weather.history[0].date,
                         weather.history[HISTORY_DAYS - 1].date,
                         HistorySource::ForecastFallback, hourlyChart, chartError)) {
    hourlyChartAvailable = true;
  } else {
    // The normal weather form remains usable if the optional chart data are
    // incomplete. The chart route will return an immediate, clear error.
    Serial.print("Forecast hourly chart unavailable: ");
    Serial.println(chartError);
  }

  return true;
}

bool replaceHistoryWithHistoricalForecast(const UpazilaCoordinate& location,
                                          WeatherData& weather,
                                          HourlyChartData& hourlyChart,
                                          bool& hourlyChartAvailable,
                                          String& error) {
  hourlyChartAvailable = false;
  const float latitude = location.latitudeE6 / 1000000.0f;
  const float longitude = location.longitudeE6 / 1000000.0f;
  const char* startDate = weather.history[0].date;
  const char* endDate = weather.history[HISTORY_DAYS - 1].date;

  String url = String("https://historical-forecast-api.open-meteo.com/v1/forecast?latitude=") + String(latitude, 6) +
               "&longitude=" + String(longitude, 6) +
               "&start_date=" + startDate +
               "&end_date=" + endDate +
               "&daily=precipitation_sum,precipitation_hours,uv_index_max,temperature_2m_min,temperature_2m_max" +
               "&hourly=precipitation,temperature_2m,relative_humidity_2m,uv_index" +
               "&timezone=Asia%2FDhaka&models=best_match&cell_selection=land";

  JsonDocument document;
  if (!requestJson(url, document, error)) return false;

  JsonObject daily = document["daily"].as<JsonObject>();
  JsonArray dates = daily["time"].as<JsonArray>();
  JsonArray rainfall = daily["precipitation_sum"].as<JsonArray>();
  JsonArray precipitationHours = daily["precipitation_hours"].as<JsonArray>();
  JsonArray uvIndex = daily["uv_index_max"].as<JsonArray>();
  JsonArray temperatureMin = daily["temperature_2m_min"].as<JsonArray>();
  JsonArray temperatureMax = daily["temperature_2m_max"].as<JsonArray>();

  if (daily.isNull() || dates.size() != HISTORY_DAYS || rainfall.size() != HISTORY_DAYS ||
      precipitationHours.size() != HISTORY_DAYS || uvIndex.size() != HISTORY_DAYS ||
      temperatureMin.size() != HISTORY_DAYS || temperatureMax.size() != HISTORY_DAYS) {
    error = "Historical Forecast service returned incomplete data.";
    return false;
  }

  for (uint8_t i = 0; i < HISTORY_DAYS; i++) {
    const char* returnedDate = dates[i].as<const char*>();
    if (returnedDate == nullptr || strcmp(returnedDate, weather.history[i].date) != 0) {
      error = "Historical Forecast dates did not match the requested completed-day range.";
      return false;
    }
  }

  String chartError;
  if (extractHourlyChart(document, location, startDate, endDate,
                         HistorySource::HistoricalForecast, hourlyChart, chartError)) {
    hourlyChartAvailable = true;
  } else {
    // Keep valid daily historical values even if the optional hourly graph
    // payload is unavailable; the forecast-hourly cache remains a fallback.
    Serial.print("Historical hourly chart unavailable: ");
    Serial.println(chartError);
  }

  weather.historyGridLatitude = document["latitude"].isNull() ? latitude : document["latitude"].as<float>();
  weather.historyGridLongitude = document["longitude"].isNull() ? longitude : document["longitude"].as<float>();
  weather.previous7CompletedDaysRainfallMm = 0.0f;
  weather.historySource = HistorySource::HistoricalForecast;

  for (uint8_t i = 0; i < HISTORY_DAYS; i++) {
    weather.history[i].rainfallMm = rainfall[i].as<float>();
    weather.history[i].precipitationHours = precipitationHours[i].as<float>();
    weather.history[i].uvIndexMax = uvIndex[i].as<float>();
    weather.history[i].temperatureMinC = temperatureMin[i].as<float>();
    weather.history[i].temperatureMaxC = temperatureMax[i].as<float>();
    weather.previous7CompletedDaysRainfallMm += weather.history[i].rainfallMm;
  }

  return true;
}

bool getWeatherForLocation(int upazilaIndex, WeatherData& weather, String& error) {
  if (upazilaIndex < 0 || upazilaIndex >= static_cast<int>(BUILTIN_UPAZILA_COUNT)) {
    error = "Invalid built-in Upazila location.";
    return false;
  }

  bool cacheMatches = weatherCacheValid &&
                      cachedUpazilaIndex == upazilaIndex &&
                      (millis() - weatherCacheUpdatedAt) < WEATHER_CACHE_DURATION_MS;

  if (cacheMatches) {
    weather = cachedWeather;
    return true;
  }

  const UpazilaCoordinate& location = kBangladeshUpazilaCoordinates[upazilaIndex];
  // Refreshing a weather location invalidates the old hourly cache. The one
  // global scratch buffer avoids placing 1 KB chart objects on loopTask.
  hourlyChartCacheValid = false;
  cachedHourlyChartUpazilaIndex = -1;

  bool forecastHourlyChartAvailable = false;
  if (!fetchCurrentAndFallbackHistory(location, weather, weatherFetchHourlyChart,
                                      forecastHourlyChartAvailable, error)) {
    return false;
  }

  if (forecastHourlyChartAvailable) {
    cachedHourlyChart = weatherFetchHourlyChart;
    cachedHourlyChartUpazilaIndex = upazilaIndex;
    hourlyChartCacheUpdatedAt = millis();
    hourlyChartCacheValid = true;
  }

  bool historicalHourlyChartAvailable = false;
  String historyError;
  if (!replaceHistoryWithHistoricalForecast(location, weather, weatherFetchHourlyChart,
                                            historicalHourlyChartAvailable, historyError)) {
    // Never hide a fallback as historical data. The web UI and CSV record the
    // source explicitly, while the current weather request remains usable.
    Serial.print("Historical Forecast unavailable; using forecast fallback: ");
    Serial.println(historyError);
  }

  // The graph endpoint is intentionally cache-only. This makes the browser's
  // automatic chart request local and prevents extra serial HTTPS requests.
  if (historicalHourlyChartAvailable) {
    cachedHourlyChart = weatherFetchHourlyChart;
    cachedHourlyChartUpazilaIndex = upazilaIndex;
    hourlyChartCacheUpdatedAt = millis();
    hourlyChartCacheValid = true;
  }

  cachedWeather = weather;
  cachedUpazilaIndex = upazilaIndex;
  weatherCacheUpdatedAt = millis();
  weatherCacheValid = true;
  return true;
}

bool scaleHourlyChartValue(float value, int16_t& scaledValue) {
  if (!isfinite(value)) return false;
  const float tenths = value * 10.0f;
  if (tenths < -32767.0f || tenths > 32767.0f) return false;
  scaledValue = static_cast<int16_t>(lroundf(tenths));
  return scaledValue != HOURLY_CHART_MISSING;
}

bool extractHourlyChart(JsonDocument& document,
                        const UpazilaCoordinate& location,
                        const char* startDate,
                        const char* endDate,
                        HistorySource source,
                        HourlyChartData& chart,
                        String& error) {
  JsonObject hourly = document["hourly"].as<JsonObject>();
  JsonArray timestamps = hourly["time"].as<JsonArray>();
  JsonArray rainfall = hourly["precipitation"].as<JsonArray>();
  JsonArray temperature = hourly["temperature_2m"].as<JsonArray>();
  JsonArray humidity = hourly["relative_humidity_2m"].as<JsonArray>();
  JsonArray uvIndex = hourly["uv_index"].as<JsonArray>();

  if (hourly.isNull() || timestamps.isNull() || rainfall.isNull() ||
      temperature.isNull() || humidity.isNull() || timestamps.size() != rainfall.size() ||
      timestamps.size() != temperature.size() || timestamps.size() != humidity.size()) {
    error = "Hourly chart service returned incomplete data.";
    return false;
  }

  // UV is optional: a missing or incorrectly sized array must not invalidate
  // rainfall, temperature, or humidity, nor shift values to the wrong hours.
  const bool uvIndexAligned = !uvIndex.isNull() && uvIndex.size() == timestamps.size();

  memset(&chart, 0, sizeof(chart));
  for (uint16_t index = 0; index < HOURLY_CHART_POINTS; index++) {
    chart.samples[index].rainfallTenthsMm = HOURLY_CHART_MISSING;
    chart.samples[index].temperatureTenthsC = HOURLY_CHART_MISSING;
    chart.samples[index].uvIndexTenths = HOURLY_CHART_MISSING;
    chart.samples[index].relativeHumidityPercent = HOURLY_HUMIDITY_MISSING;
  }

  uint16_t sampleCount = 0;
  for (size_t index = 0; index < timestamps.size(); index++) {
    const char* timestamp = timestamps[index].as<const char*>();
    if (timestamp == nullptr || strlen(timestamp) < 13) {
      error = "Hourly chart service returned an invalid time.";
      return false;
    }

    // The forecast fallback includes today. Keep only the same seven completed
    // Bangladesh dates used by the existing daily historical table.
    if (strncmp(timestamp, startDate, 10) < 0 || strncmp(timestamp, endDate, 10) > 0) continue;
    if (sampleCount >= HOURLY_CHART_POINTS || rainfall[index].isNull() ||
        temperature[index].isNull() || humidity[index].isNull()) {
      error = "Hourly chart service returned invalid values.";
      return false;
    }

    const float rainValue = rainfall[index].as<float>();
    const float temperatureValue = temperature[index].as<float>();
    const float humidityValue = humidity[index].as<float>();
    const long roundedHumidity = lroundf(humidityValue);

    if (rainValue < 0.0f || !scaleHourlyChartValue(rainValue, chart.samples[sampleCount].rainfallTenthsMm) ||
        !scaleHourlyChartValue(temperatureValue, chart.samples[sampleCount].temperatureTenthsC) ||
        !isfinite(humidityValue) || roundedHumidity < 0 || roundedHumidity > 100) {
      error = "Hourly chart service returned out-of-range values.";
      return false;
    }

    chart.samples[sampleCount].relativeHumidityPercent = static_cast<uint8_t>(roundedHumidity);
    if (uvIndexAligned && uvIndex[index].is<float>()) {
      const float uvValue = uvIndex[index].as<float>();
      int16_t scaledUvValue;
      if (uvValue >= 0.0f && scaleHourlyChartValue(uvValue, scaledUvValue)) {
        chart.samples[sampleCount].uvIndexTenths = scaledUvValue;
      }
    }
    sampleCount++;
  }

  if (sampleCount != HOURLY_CHART_POINTS) {
    error = "Hourly chart service did not return seven complete days.";
    return false;
  }

  copyText(chart.startDate, sizeof(chart.startDate), startDate);
  setRecordTimestamp(chart.fetchedAt, sizeof(chart.fetchedAt));
  chart.gridLatitude = document["latitude"].isNull() ? location.latitudeE6 / 1000000.0f : document["latitude"].as<float>();
  chart.gridLongitude = document["longitude"].isNull() ? location.longitudeE6 / 1000000.0f : document["longitude"].as<float>();
  chart.source = source;
  return true;
}

void storeRecord(const FieldData& record) {
  if (logCount < MAX_LOG_RECORDS) {
    size_t index = (logStart + logCount) % MAX_LOG_RECORDS;
    logRecords[index] = record;
    logCount++;
  } else {
    // Once full, replace the oldest record.
    logRecords[logStart] = record;
    logStart = (logStart + 1) % MAX_LOG_RECORDS;
  }
}

String htmlEscape(const char* text) {
  String escaped;
  if (text == nullptr) return escaped;

  for (size_t i = 0; text[i] != '\0'; i++) {
    switch (text[i]) {
      case '&': escaped += "&amp;"; break;
      case '<': escaped += "&lt;"; break;
      case '>': escaped += "&gt;"; break;
      case '\"': escaped += "&quot;"; break;
      case '\'': escaped += "&#39;"; break;
      default: escaped += text[i]; break;
    }
  }

  return escaped;
}

void appendComma(String& row) {
  if (row.length() > 0) row += ',';
}

void appendCsvText(String& row, const char* value) {
  appendComma(row);
  String cell = value == nullptr ? "" : String(value);

  // Prevent spreadsheet formula execution for free-text fields.
  if (cell.length() > 0 && (cell[0] == '=' || cell[0] == '+' || cell[0] == '-' || cell[0] == '@')) {
    cell = String("'") + cell;
  }

  row += '"';
  for (size_t i = 0; i < cell.length(); i++) {
    if (cell[i] == '\"') row += "\"\"";
    else row += cell[i];
  }
  row += '"';
}

void appendCsvUnsigned(String& row, uint32_t value) {
  appendComma(row);
  row += String(value);
}

void appendCsvInteger(String& row, int value) {
  appendComma(row);
  row += String(value);
}

void appendCsvFloat(String& row, float value, uint8_t decimals = 2) {
  appendComma(row);
  if (isfinite(value)) row += String(value, static_cast<unsigned int>(decimals));
}

String formatPlotAreaM2(double value) {
  // Keep large fractional areas and small positive areas numeric, without
  // reducing the new double field to float or rounding it to zero at 2 dp.
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%.15g", value);
  return String(buffer);
}

void appendCsvPlotArea(String& row, double value) {
  appendComma(row);
  if (isfinite(value) && value > 0.0) row += formatPlotAreaM2(value);
}

String buildCsvHeader() {
  String header =
    "weather_schema_version,record_id,district,upazila,saved_at,block,plot_id,plot_area_m2,crop,variety,soil_type,latitude,longitude,"
    "coordinate_basis,coordinate_dataset_version,forecast_grid_latitude,forecast_grid_longitude,"
    "history_grid_latitude,history_grid_longitude,weather_provider,current_data_type,"
    "history_source,history_data_type,history_window,"
    "water_level_mm,tensiometer_cb,pump_on,water_level_source,jsn_distance_mm,jsn_reference_height_mm,jsn_water_level_mm,"
    "current_temperature_c,current_relative_humidity_pct,current_cloud_cover_pct,"
    "current_precipitation_15_min_mm,current_uv_index,current_weather_code,current_weather_summary,weather_time,"
    "previous_7_completed_days_rainfall_mm";

  for (uint8_t i = 0; i < HISTORY_DAYS; i++) {
    uint8_t day = i + 1;
    header += ",history_day_";
    header += String(day);
    header += "_date";
    header += ",history_day_";
    header += String(day);
    header += "_rainfall_mm";
    header += ",history_day_";
    header += String(day);
    header += "_precipitation_hours";
    header += ",history_day_";
    header += String(day);
    header += "_uv_index_max";
    header += ",history_day_";
    header += String(day);
    header += "_temperature_min_c";
    header += ",history_day_";
    header += String(day);
    header += "_temperature_max_c";
  }

  header += "\n";
  return header;
}

String buildCsvRow(const FieldData& record) {
  String row;
  row.reserve(2000);

  appendCsvText(row, "5");
  appendCsvUnsigned(row, record.recordId);
  appendCsvText(row, record.district);
  appendCsvText(row, record.upazila);
  appendCsvText(row, record.savedAt);
  appendCsvText(row, record.block);
  appendCsvText(row, record.plotId);
  appendCsvPlotArea(row, record.plotAreaM2);
  appendCsvText(row, record.crop);
  appendCsvText(row, record.variety);
  appendCsvText(row, record.soilType);
  appendCsvFloat(row, record.weather.latitude, 6);
  appendCsvFloat(row, record.weather.longitude, 6);
  appendCsvText(row, coordinateBasisCsvText(record.weather.coordinateBasis));
  appendCsvText(row, BUILTIN_UPAZILA_DATASET_VERSION);
  appendCsvFloat(row, record.weather.forecastGridLatitude, 6);
  appendCsvFloat(row, record.weather.forecastGridLongitude, 6);
  appendCsvFloat(row, record.weather.historyGridLatitude, 6);
  appendCsvFloat(row, record.weather.historyGridLongitude, 6);
  appendCsvText(row, "Open-Meteo");
  appendCsvText(row, "forecast_grid_model_15_min_estimate");
  appendCsvText(row, historySourceText(record.weather.historySource));
  appendCsvText(row, historyDataTypeText(record.weather.historySource));
  appendCsvText(row, "previous_7_completed_days");
  appendCsvFloat(row, record.irrigationWaterLevelMm);
  appendCsvFloat(row, record.tensiometerCb);
  appendCsvInteger(row, record.pumpOn ? 1 : 0);
  appendCsvText(row, record.waterFromSensor ? "jsn_sr04t" : "manual");
  appendCsvFloat(row, record.waterSensorDistanceMm);
  appendCsvFloat(row, record.waterReferenceHeightMm);
  appendCsvFloat(row, record.waterSensorLevelMm);
  appendCsvFloat(row, record.weather.currentTemperatureC);
  appendCsvFloat(row, record.weather.relativeHumidityPercent);
  appendCsvFloat(row, record.weather.cloudCoverPercent);
  appendCsvFloat(row, record.weather.currentPrecipitationMm);
  appendCsvFloat(row, record.weather.currentUvIndex);
  appendCsvInteger(row, record.weather.currentWeatherCode);
  appendCsvText(row, currentWeatherSummaryBangladesh(record.weather.currentWeatherCode));
  appendCsvText(row, record.weather.reportedAt);
  appendCsvFloat(row, record.weather.previous7CompletedDaysRainfallMm);

  for (uint8_t i = 0; i < HISTORY_DAYS; i++) {
    appendCsvText(row, record.weather.history[i].date);
    appendCsvFloat(row, record.weather.history[i].rainfallMm);
    appendCsvFloat(row, record.weather.history[i].precipitationHours);
    appendCsvFloat(row, record.weather.history[i].uvIndexMax);
    appendCsvFloat(row, record.weather.history[i].temperatureMinC);
    appendCsvFloat(row, record.weather.history[i].temperatureMaxC);
  }

  row += "\n";
  return row;
}

void setRecordTimestamp(char* destination, size_t destinationSize) {
  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 100)) {
    strftime(destination, destinationSize, "%Y-%m-%d %H:%M:%S", &timeInfo);
  } else {
    copyText(destination, destinationSize, "Unknown");
  }
}

// Keep ISO 8601 internally and in CSV so Excel can sort it reliably, while
// presenting the Bangladesh/Malaysia-friendly day/month/year order in HTML.
String formatRegionalDateTime(const char* isoDateTime) {
  if (isoDateTime == nullptr || strlen(isoDateTime) < 10 ||
      isoDateTime[4] != '-' || isoDateTime[7] != '-') {
    return String(isoDateTime == nullptr ? "Unknown" : isoDateTime);
  }

  String formatted;
  formatted.reserve(strlen(isoDateTime) + 1);
  formatted += isoDateTime[8];
  formatted += isoDateTime[9];
  formatted += '/';
  formatted += isoDateTime[5];
  formatted += isoDateTime[6];
  formatted += '/';
  formatted += isoDateTime[0];
  formatted += isoDateTime[1];
  formatted += isoDateTime[2];
  formatted += isoDateTime[3];

  if (strlen(isoDateTime) > 10) {
    formatted += isoDateTime[10] == 'T' ? ' ' : isoDateTime[10];
    formatted += isoDateTime + 11;
  }

  return formatted;
}

String confirmationPage(const FieldData& record) {
  String page;
  page.reserve(4000);
  const String displaySavedAt = formatRegionalDateTime(record.savedAt);
  page += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  page += "<title>Agro-advisory field Toolkit</title><style>";
  page += "body{margin:0;padding:25px 15px;font-family:Arial,sans-serif;background:#f2f7f2;color:#193b24;}";
  page += ".card{max-width:560px;margin:auto;padding:25px;background:#fff;border-radius:15px;box-shadow:0 4px 18px rgba(0,0,0,.15);}";
  page += "h2{text-align:center;color:#167342;}table{width:100%;border-collapse:collapse;margin:18px 0;}td{padding:10px;border-bottom:1px solid #dbe8dc;}td:first-child{font-weight:bold;color:#167342;width:50%;}";
  page += "a{display:block;padding:14px;margin-top:14px;border-radius:8px;background:#16874a;color:#fff;text-align:center;text-decoration:none;font-weight:bold;}.download{background:#5d4aa8;}</style></head><body><div class='card'>";
  page += "<h2>Field Information Saved</h2><table>";
  page += "<tr><td>District</td><td>";
  page += htmlEscape(record.district);
  page += "</td></tr>";
  page += "<tr><td>Upazila</td><td>";
  page += htmlEscape(record.upazila);
  page += "</td></tr>";
  page += "<tr><td>Saved at</td><td>";
  page += htmlEscape(displaySavedAt.c_str());
  page += "</td></tr>";
  page += "<tr><td>Block</td><td>";
  page += htmlEscape(record.block);
  page += "</td></tr>";
  page += "<tr><td>Plot ID</td><td>";
  page += htmlEscape(record.plotId);
  page += "</td></tr>";
  page += "<tr><td>Plot Area</td><td>";
  page += formatPlotAreaM2(record.plotAreaM2);
  page += " m&sup2;</td></tr>";
  page += "<tr><td>Crop</td><td>";
  page += htmlEscape(record.crop);
  page += "</td></tr>";
  page += "<tr><td>Variety</td><td>";
  page += htmlEscape(record.variety);
  page += "</td></tr>";
  page += "<tr><td>Soil Type</td><td>";
  page += htmlEscape(record.soilType);
  page += "</td></tr>";
  page += "<tr><td>Water level</td><td>";
  page += String(record.irrigationWaterLevelMm, 2);
  page += " mm</td></tr>";
  page += "<tr><td>Water-level source</td><td>";
  page += record.waterFromSensor ? "JSN-SR04T calibrated surface estimate" : "Manual field measurement";
  page += "</td></tr>";
  page += "<tr><td>Tensiometer</td><td>";
  page += String(record.tensiometerCb, 2);
  page += " cb</td></tr>";
  page += "<tr><td>Current irrigation pump state</td><td>";
  page += (record.pumpOn ? "ON" : "OFF");
  page += "</td></tr>";
  page += "<tr><td>Current model weather summary</td><td>";
  page += currentWeatherSummaryBangladesh(record.weather.currentWeatherCode);
  page += " (WMO code ";
  page += String(record.weather.currentWeatherCode);
  page += ")</td></tr>";
  page += "<tr><td>Current data type</td><td>15-minute forecast grid-model estimate, not a station observation</td></tr>";
  page += "<tr><td>Historical data source</td><td>";
  page += historySourceText(record.weather.historySource);
  page += "<br>";
  page += historyDataTypeText(record.weather.historySource);
  page += "</td></tr>";
  page += "<tr><td>Upazila reference coordinate</td><td>";
  page += coordinateBasisText(record.weather.coordinateBasis);
  page += "<br>";
  page += String(record.weather.latitude, 6);
  page += ", ";
  page += String(record.weather.longitude, 6);
  page += "</td></tr>";
  page += "<tr><td>Current model temperature</td><td>";
  page += String(record.weather.currentTemperatureC, 1);
  page += " &deg;C</td></tr>";
  page += "<tr><td>Previous 7 completed-day rainfall</td><td>";
  page += String(record.weather.previous7CompletedDaysRainfallMm, 1);
  page += " mm</td></tr>";
  page += "<tr><td>Model current UV index</td><td>";
  page += String(record.weather.currentUvIndex, 1);
  page += "</td></tr>";
  page += "</table><p style='font-size:13px;color:#64756a;text-align:center;'>Historical rows exclude daily WMO condition codes. They are completed-day grid estimates, not farm observations.</p><a href='/'>Enter Another Record</a><a class='download' href='/download'>Download CSV Log</a></div></body></html>";
  return page;
}

void sendJsonError(int statusCode, const String& error) {
  JsonDocument document;
  document["ok"] = false;
  document["error"] = error;
  String response;
  serializeJson(document, response);
  server.send(statusCode, "application/json; charset=utf-8", response);
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", htmlPage);
}

void handleDistricts() {
  JsonDocument document;
  document["ok"] = true;
  JsonArray districts = document["districts"].to<JsonArray>();

  const char* previousDistrict = nullptr;
  for (size_t index = 0; index < BUILTIN_UPAZILA_COUNT; index++) {
    const char* district = kBangladeshUpazilaCoordinates[index].district;
    if (previousDistrict == nullptr || strcmp(previousDistrict, district) != 0) {
      districts.add(district);
      previousDistrict = district;
    }
  }

  document["count"] = districts.size();
  String response;
  serializeJson(document, response);
  server.send(200, "application/json; charset=utf-8", response);
}

void handleUpazilas() {
  char districtInput[DISTRICT_TEXT_LENGTH];
  String error;

  if (!parseTextArg("district", "District", districtInput, sizeof(districtInput), error)) {
    sendJsonError(400, error);
    return;
  }

  const char* district = findCanonicalDistrict(districtInput);
  if (district == nullptr) {
    sendJsonError(404, "District is not in the built-in 503-Upazila Bangladesh table.");
    return;
  }

  JsonDocument document;
  document["ok"] = true;
  document["district"] = district;
  JsonArray upazilas = document["upazilas"].to<JsonArray>();

  for (size_t index = 0; index < BUILTIN_UPAZILA_COUNT; index++) {
    const UpazilaCoordinate& entry = kBangladeshUpazilaCoordinates[index];
    if (strcmp(entry.district, district) == 0) upazilas.add(entry.upazila);
  }

  document["count"] = upazilas.size();
  String response;
  serializeJson(document, response);
  server.send(200, "application/json; charset=utf-8", response);
}

void handleWeather() {
  char district[DISTRICT_TEXT_LENGTH];
  char upazila[UPAZILA_TEXT_LENGTH];
  String error;

  if (!parseTextArg("district", "District", district, sizeof(district), error) ||
      !parseTextArg("upazila", "Upazila", upazila, sizeof(upazila), error)) {
    sendJsonError(400, error);
    return;
  }

  int upazilaIndex = findUpazilaIndex(district, upazila);
  if (upazilaIndex < 0) {
    sendJsonError(404, "No exact District and Upazila match in the built-in 503-Upazila Bangladesh table. Select values from the lists.");
    return;
  }

  const UpazilaCoordinate& location = kBangladeshUpazilaCoordinates[upazilaIndex];
  WeatherData& weather = weatherResponseScratch;
  if (!getWeatherForLocation(upazilaIndex, weather, error)) {
    sendJsonError(503, error);
    return;
  }

  JsonDocument document;
  document["ok"] = true;
  document["district"] = location.district;
  document["upazila"] = location.upazila;
  document["latitude"] = weather.latitude;
  document["longitude"] = weather.longitude;
  document["location_reference"] = coordinateBasisText(weather.coordinateBasis);
  document["location_dataset_version"] = BUILTIN_UPAZILA_DATASET_VERSION;
  document["weather_provider"] = "Open-Meteo";
  document["forecast_grid_latitude"] = weather.forecastGridLatitude;
  document["forecast_grid_longitude"] = weather.forecastGridLongitude;
  document["history_grid_latitude"] = weather.historyGridLatitude;
  document["history_grid_longitude"] = weather.historyGridLongitude;
  document["current_data_type"] = "15-minute forecast grid-model estimate";
  document["history_source"] = historySourceText(weather.historySource);
  document["history_data_type"] = historyDataTypeText(weather.historySource);
  document["current_weather_code"] = weather.currentWeatherCode;
  document["current_weather_summary"] = currentWeatherSummaryBangladesh(weather.currentWeatherCode);
  document["weather_time"] = weather.reportedAt;
  document["temperature_c"] = weather.currentTemperatureC;
  document["relative_humidity_percent"] = weather.relativeHumidityPercent;
  document["cloud_cover_percent"] = weather.cloudCoverPercent;
  document["current_precipitation_15_min_mm"] = weather.currentPrecipitationMm;
  document["current_uv_index"] = weather.currentUvIndex;
  document["previous_7_completed_days_rainfall_mm"] = weather.previous7CompletedDaysRainfallMm;

  JsonArray history = document["history"].to<JsonArray>();
  for (uint8_t i = 0; i < HISTORY_DAYS; i++) {
    JsonObject day = history.add<JsonObject>();
    day["date"] = weather.history[i].date;
    day["rainfall_mm"] = weather.history[i].rainfallMm;
    day["precipitation_hours"] = weather.history[i].precipitationHours;
    day["uv_index_max"] = weather.history[i].uvIndexMax;
    day["temperature_min_c"] = weather.history[i].temperatureMinC;
    day["temperature_max_c"] = weather.history[i].temperatureMaxC;
  }

  String response;
  serializeJson(document, response);
  server.send(200, "application/json; charset=utf-8", response);
}

void streamHourlyChartArray(const HourlyChartData& chart, uint8_t valueType) {
  String chunk;
  chunk.reserve(512);

  for (uint16_t index = 0; index < HOURLY_CHART_POINTS; index++) {
    if (index > 0) chunk += ',';

    char number[16];
    if (valueType == 0) {
      const int16_t value = chart.samples[index].rainfallTenthsMm;
      if (value == HOURLY_CHART_MISSING) chunk += "null";
      else {
        snprintf(number, sizeof(number), "%d", value);
        chunk += number;
      }
    } else if (valueType == 1) {
      const int16_t value = chart.samples[index].temperatureTenthsC;
      if (value == HOURLY_CHART_MISSING) chunk += "null";
      else {
        snprintf(number, sizeof(number), "%d", value);
        chunk += number;
      }
    } else if (valueType == 2) {
      const uint8_t value = chart.samples[index].relativeHumidityPercent;
      if (value == HOURLY_HUMIDITY_MISSING) chunk += "null";
      else {
        snprintf(number, sizeof(number), "%u", static_cast<unsigned int>(value));
        chunk += number;
      }
    } else {
      const int16_t value = chart.samples[index].uvIndexTenths;
      if (value == HOURLY_CHART_MISSING) chunk += "null";
      else {
        snprintf(number, sizeof(number), "%d", value);
        chunk += number;
      }
    }

    if (chunk.length() >= 420) {
      server.sendContent(chunk);
      chunk = "";
    }
  }

  if (chunk.length() > 0) server.sendContent(chunk);
}

void sendHourlyChartJson(const HourlyChartData& chart) {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json; charset=utf-8", "");

  String metadata;
  metadata.reserve(420);
  metadata += "{\"ok\":true,\"start_date\":\"";
  metadata += chart.startDate;
  metadata += "\",\"sample_count\":";
  metadata += String(static_cast<unsigned int>(HOURLY_CHART_POINTS));
  metadata += ",\"timezone\":\"Asia/Dhaka\",\"history_source\":\"";
  metadata += historySourceText(chart.source);
  metadata += "\",\"history_data_type\":\"";
  metadata += historyDataTypeText(chart.source);
  metadata += "\",\"chart_fetched_at\":\"";
  metadata += chart.fetchedAt;
  metadata += "\",\"grid_latitude\":";
  metadata += String(chart.gridLatitude, 6);
  metadata += ",\"grid_longitude\":";
  metadata += String(chart.gridLongitude, 6);
  metadata += ",\"rainfall_tenths_mm\":[";
  server.sendContent(metadata);
  streamHourlyChartArray(chart, 0);
  server.sendContent("],\"temperature_tenths_c\":[");
  streamHourlyChartArray(chart, 1);
  server.sendContent("],\"relative_humidity_percent\":[");
  streamHourlyChartArray(chart, 2);
  server.sendContent("],\"uv_index_tenths\":[");
  streamHourlyChartArray(chart, 3);
  server.sendContent("]}");
}

void handleWeatherChart() {
  char district[DISTRICT_TEXT_LENGTH];
  char upazila[UPAZILA_TEXT_LENGTH];
  String error;

  if (!parseTextArg("district", "District", district, sizeof(district), error) ||
      !parseTextArg("upazila", "Upazila", upazila, sizeof(upazila), error)) {
    sendJsonError(400, error);
    return;
  }

  const int upazilaIndex = findUpazilaIndex(district, upazila);
  if (upazilaIndex < 0) {
    sendJsonError(404, "No exact District and Upazila match in the built-in 503-Upazila Bangladesh table. Select values from the lists.");
    return;
  }

  const bool cacheMatches = hourlyChartCacheValid &&
                            cachedHourlyChartUpazilaIndex == upazilaIndex &&
                            (millis() - hourlyChartCacheUpdatedAt) < WEATHER_CACHE_DURATION_MS;
  if (!cacheMatches) {
    sendJsonError(409, "Refresh weather information to prepare the hourly graph.");
    return;
  }

  sendHourlyChartJson(cachedHourlyChart);
}

// Resolve live JSN data server-side; never trust a browser's claimed sensor value.
bool resolveWaterSource(FieldData& record, String& error, JsonObjectConst water) {
  const String source = server.arg("water_source");
  if (source.length() > 0 && source != "manual" && source != "jsn") {
    error = "Invalid water-level source."; return false;
  }
  record.waterFromSensor = source == "jsn";
  record.waterSensorDistanceMm = NAN;
  record.waterReferenceHeightMm = NAN;
  record.waterSensorLevelMm = NAN;
  if (water["water_reference_height_mm"].is<float>())
    record.waterReferenceHeightMm = water["water_reference_height_mm"].as<float>();
  if (water["water_sensor_ok"] == true && water["water_distance_mm"].is<float>())
    record.waterSensorDistanceMm = water["water_distance_mm"].as<float>();
  if (water["water_level_valid"] == true && water["water_level_mm"].is<float>())
    record.waterSensorLevelMm = water["water_level_mm"].as<float>();
  if (record.waterFromSensor) {
    if (!isfinite(record.waterSensorLevelMm) || record.waterSensorLevelMm < 0.0f) {
      error = "JSN water level unavailable. Check calibration and the current reading, or enter a manual measurement.";
      return false;
    }
    record.irrigationWaterLevelMm = record.waterSensorLevelMm;
  }
  return true;
}

bool resolveWaterSource(FieldData& record, String& error) {
  JsonDocument water;
  LocalWaterSensor::appendSnapshot(water.to<JsonObject>());
  return resolveWaterSource(record, error, water.as<JsonObjectConst>());
}

void handleAdvisoryStatus() {
  JsonDocument result;
  IrrigationAdvisory::appendStatus(result.to<JsonObject>());
  String body;
  serializeJson(result, body);
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(200, "application/json; charset=utf-8", body);
}

void handleAdvisoryRequest() {
  // Browser same-origin guard, not authentication. Do not expose this LAN server.
  if (server.header("X-EFB-Request") != "1") {
    sendJsonError(403, "Use the Groq review button on this device."); return;
  }
  String contentType = server.header("Content-Type");
  const int separator = contentType.indexOf(';');
  if (separator >= 0) contentType = contentType.substring(0, separator);
  contentType.trim();
  if (!contentType.equalsIgnoreCase("application/x-www-form-urlencoded")) {
    sendJsonError(415, "A form-encoded field snapshot is required."); return;
  }
  const String length = server.header("Content-Length");
  if (length.length() == 0 || length.length() > 4) {
    sendJsonError(413, "Invalid field snapshot size."); return;
  }
  size_t bytes = 0;
  for (size_t i = 0; i < length.length(); ++i) {
    if (length[i] < '0' || length[i] > '9') { sendJsonError(400, "Invalid request size."); return; }
    bytes = bytes * 10 + static_cast<size_t>(length[i] - '0');
  }
  if (bytes == 0 || bytes > 2048 || server.args() > 12) {
    sendJsonError(413, "Field snapshot is too large."); return;
  }
  const char* allowed[] = {"district", "upazila", "block", "plot_id", "plot_area_m2", "crop", "variety",
                          "soil_type", "water", "tensiometer", "pump", "water_source"};
  for (int i = 0; i < server.args(); ++i) {
    bool known = false;
    for (const char* name : allowed) if (server.argName(i) == name) known = true;
    for (int j = 0; j < i; ++j) if (server.argName(i) == server.argName(j)) known = false;
    if (!known) { sendJsonError(400, "Unknown or duplicate field."); return; }
  }
  FieldData& record = submittedRecordScratch;
  memset(&record, 0, sizeof(record));
  String error;
  JsonDocument waterSnapshot;
  LocalWaterSensor::appendSnapshot(waterSnapshot.to<JsonObject>());
  if (!parseTextArg("district", "District", record.district, sizeof(record.district), error) ||
      !parseTextArg("upazila", "Upazila", record.upazila, sizeof(record.upazila), error) ||
      !parseManualFieldInputs(record, error) ||
      !resolveWaterSource(record, error, waterSnapshot.as<JsonObjectConst>())) {
    sendJsonError(400, error); return;
  }
  const int locationIndex = findUpazilaIndex(record.district, record.upazila);
  if (locationIndex < 0) { sendJsonError(400, "Select an exact District and Upazila from the lists."); return; }
  const UpazilaCoordinate& location = kBangladeshUpazilaCoordinates[locationIndex];
  JsonDocument context;
  context["schema_version"] = 1;
  context["captured_uptime_ms"] = millis();
  char timestamp[TIMESTAMP_TEXT_LENGTH];
  setRecordTimestamp(timestamp, sizeof(timestamp));
  context["captured_at"] = timestamp;
  context["timezone"] = "Asia/Dhaka";
  JsonObject field = context["field"].to<JsonObject>();
  field["district"] = location.district; field["upazila"] = location.upazila;
  field["block"] = record.block; field["plot_id"] = record.plotId;
  field["plot_area_m2"] = record.plotAreaM2;
  field["crop"] = record.crop; field["variety"] = record.variety;
  field["soil_type"] = record.soilType;
  field["water_level_mm"] = record.irrigationWaterLevelMm;
  field["water_level_source"] = record.waterFromSensor ? "jsn_sr04t_surface_estimate" : "manual_spot_measurement";
  field["tensiometer_cb"] = record.tensiometerCb;
  field["tensiometer_source"] = "manual_spot_measurement; time and depth not supplied";
  field["pump_on_reported"] = record.pumpOn;
  field["pump_state_source"] = "manual_report; not live electrical or flow feedback";
  JsonObject local = context["local_device_sensors"].to<JsonObject>();
  LocalSensor::appendSnapshot(local);
  // Reuse the same JSN measurement that supplied field.water_level_mm.
  for (JsonPairConst item : waterSnapshot.as<JsonObjectConst>()) local[item.key()] = item.value();
  local["location_match"] = "not_verified_against_selected_plot";
  JsonObject online = context["online_weather"].to<JsonObject>();
  const bool matching = weatherCacheValid && cachedUpazilaIndex == locationIndex &&
                        millis() - weatherCacheUpdatedAt < WEATHER_CACHE_DURATION_MS;
  online["available"] = matching;
  online["provider"] = "Open-Meteo";
  if (matching) {
    const WeatherData& weather = cachedWeather;
    online["cache_age_ms"] = millis() - weatherCacheUpdatedAt;
    online["reported_at"] = weather.reportedAt;
    online["data_type"] = "model_estimates_not_field_observations";
    online["latitude"] = weather.latitude; online["longitude"] = weather.longitude;
    online["coordinate_basis"] = coordinateBasisText(weather.coordinateBasis);
    online["temperature_c"] = weather.currentTemperatureC;
    online["relative_humidity_pct"] = weather.relativeHumidityPercent;
    online["cloud_cover_pct"] = weather.cloudCoverPercent;
    online["precipitation_15_min_mm"] = weather.currentPrecipitationMm;
    online["uv_index"] = weather.currentUvIndex;
    online["weather_code"] = weather.currentWeatherCode;
    online["previous_7_completed_days_rainfall_mm"] = weather.previous7CompletedDaysRainfallMm;
    online["history_source"] = historySourceText(weather.historySource);
    JsonArray history = online["history"].to<JsonArray>();
    for (uint8_t i = 0; i < HISTORY_DAYS; ++i) {
      JsonObject day = history.add<JsonObject>();
      day["date"] = weather.history[i].date;
      day["rainfall_mm"] = weather.history[i].rainfallMm;
      day["precipitation_hours"] = weather.history[i].precipitationHours;
      day["uv_max"] = weather.history[i].uvIndexMax;
      day["temperature_min_c"] = weather.history[i].temperatureMinC;
      day["temperature_max_c"] = weather.history[i].temperatureMaxC;
    }
  } else online["reason"] = "No fresh weather cache for this selected location; do not infer weather.";
  JsonObject policy = context["deterministic_advisory"].to<JsonObject>();
  policy["status"] = "review_required";
  policy["irrigation_minutes"] = nullptr;
  policy["pump_control_enabled"] = false;
  JsonArray missing = policy["missing_configuration"].to<JsonArray>();
  missing.add("crop_growth_stage"); missing.add("validated_local_irrigation_thresholds");
  missing.add("irrigation_method_and_root_zone_or_paddy_management");
  missing.add("measured_pump_delivery_flow_l_min"); missing.add("application_efficiency_and_target_net_depth");
  if (strcmp(record.soilType, "Unknown") == 0) missing.add("soil_type");
  if (context.overflowed()) { sendJsonError(503, "Insufficient memory to prepare a field review."); return; }
  if (!IrrigationAdvisory::request(context, error)) { sendJsonError(503, error); return; }
  handleAdvisoryStatus();  // Worker owns a copy. This endpoint does not save a record.
}

void handleSubmit() {
  FieldData& record = submittedRecordScratch;
  memset(&record, 0, sizeof(record));
  String error;

  if (!parseTextArg("district", "District", record.district, sizeof(record.district), error) ||
      !parseTextArg("upazila", "Upazila", record.upazila, sizeof(record.upazila), error)) {
    server.send(400, "text/plain", error);
    return;
  }

  int upazilaIndex = findUpazilaIndex(record.district, record.upazila);
  if (upazilaIndex < 0) {
    server.send(400, "text/plain", "No exact District and Upazila match in the built-in 503-Upazila Bangladesh table. Select values from the lists.");
    return;
  }

  // Store canonical table labels rather than browser-entered spelling variants.
  const UpazilaCoordinate& location = kBangladeshUpazilaCoordinates[upazilaIndex];
  copyText(record.district, sizeof(record.district), location.district);
  copyText(record.upazila, sizeof(record.upazila), location.upazila);

  if (!parseManualFieldInputs(record, error)) {
    server.send(400, "text/plain", error);
    return;
  }

  // Fetch on the ESP32 so submitted weather values cannot be modified by a
  // browser. A matching built-in location uses the recent 15-minute cache.
  if (!getWeatherForLocation(upazilaIndex, record.weather, error)) {
    server.send(503, "text/plain", String("Cannot save because automatic weather data are unavailable: ") + error);
    return;
  }

  // Resolve AFTER any blocking weather transfer, so the saved JSN value is fresh.
  if (!resolveWaterSource(record, error)) { server.send(400, "text/plain", error); return; }
  record.recordId = nextRecordId++;
  setRecordTimestamp(record.savedAt, sizeof(record.savedAt));
  storeRecord(record);

  Serial.println("\n--- Field Record Saved ---");
  Serial.print("Record ID: "); Serial.println(record.recordId);
  Serial.print("Location: "); Serial.print(record.upazila); Serial.print(", "); Serial.println(record.district);
  Serial.print("Block: "); Serial.println(record.block);
  Serial.print("Plot ID: "); Serial.println(record.plotId);
  Serial.print("Plot Area: "); Serial.print(formatPlotAreaM2(record.plotAreaM2)); Serial.println(" m2");
  Serial.print("Crop: "); Serial.println(record.crop);
  Serial.print("Variety: "); Serial.println(record.variety);
  Serial.print("Soil Type: "); Serial.println(record.soilType);
  Serial.print("Built-in Coordinates: ");
  Serial.print(record.weather.latitude, 6);
  Serial.print(", ");
  Serial.println(record.weather.longitude, 6);
  Serial.print("Coordinate Basis: ");
  Serial.println(coordinateBasisText(record.weather.coordinateBasis));
  Serial.print("Water Level: "); Serial.print(record.irrigationWaterLevelMm); Serial.println(" mm");
  Serial.print("Water source: "); Serial.println(record.waterFromSensor ? "JSN-SR04T" : "manual");
  Serial.print("Tensiometer: "); Serial.print(record.tensiometerCb); Serial.println(" cb");
  Serial.print("Current Irrigation Pump State: "); Serial.println(record.pumpOn ? "ON" : "OFF");
  Serial.print("Model Weather Code: "); Serial.println(record.weather.currentWeatherCode);
  Serial.print("Current Model Weather Summary: "); Serial.println(currentWeatherSummaryBangladesh(record.weather.currentWeatherCode));
  Serial.print("Current Model Temperature: "); Serial.print(record.weather.currentTemperatureC); Serial.println(" C");
  Serial.print("Previous 7 Completed-Day Rainfall: "); Serial.print(record.weather.previous7CompletedDaysRainfallMm); Serial.println(" mm");
  Serial.print("History Source: "); Serial.println(historySourceText(record.weather.historySource));
  Serial.print("Model Current UV Index: "); Serial.println(record.weather.currentUvIndex);

  server.send(200, "text/html; charset=utf-8", confirmationPage(record));
}

void handleDownload() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"electronic_field_bag_log.csv\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");

  // Stream the CSV one row at a time instead of creating a large RAM string.
  server.sendContent(buildCsvHeader());
  for (size_t i = 0; i < logCount; i++) {
    size_t index = (logStart + i) % MAX_LOG_RECORDS;
    server.sendContent(buildCsvRow(logRecords[index]));
  }
}

void blinkTimeSyncLed() {
  if (STATUS_LED_PIN < 0) return;
  unsigned long startedAt = millis();
  while (millis() - startedAt < 2000) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    delay(250);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(250);
  }
}

void startTimeService() {
  configTime(GMT_OFFSET_SECONDS, DAYLIGHT_OFFSET_SECONDS, NTP_SERVER);
}

bool waitForTimeSynchronization(unsigned long timeoutMs) {
  struct tm timeInfo;
  unsigned long startedAt = millis();

  while (millis() - startedAt < timeoutMs) {
    if (getLocalTime(&timeInfo, 250)) return true;
    delay(250);
  }

  return false;
}

void connectToHomeWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASSWORD);

  Serial.print("Connecting to home Wi-Fi");
  unsigned long startedAt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 30000) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi connection failed. Check the name, password, and 2.4 GHz network.");
    return;
  }

  Serial.println("Connected to home Wi-Fi.");

  startTimeService();
  Serial.print("Synchronizing time");
  if (waitForTimeSynchronization(15000)) {
    Serial.println(" done.");
    blinkTimeSyncLed();
  } else {
    Serial.println(" failed. Records will show Unknown time until synchronization succeeds.");
  }
}

void maintainWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastReconnectAttempt >= 10000) {
    lastReconnectAttempt = millis();
    Serial.println("Wi-Fi disconnected. Trying to reconnect...");
    WiFi.reconnect();
  }
}

// Call only after server.begin(). Keep this state on the main Arduino task;
// the BME680 task can report failures independently without hiding the URL.
void reportWebServerAddress(bool force = false) {
  static bool serialWasReady = false;
  static bool lastWifiConnected = false;
  static IPAddress lastAddress(0, 0, 0, 0);
  static uint32_t lastReportMs = 0;

  if (!Serial) {
    serialWasReady = false;
    return;
  }

  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  const IPAddress address = wifiConnected ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
  const uint32_t now = millis();
  const bool stateChanged = wifiConnected != lastWifiConnected || address != lastAddress;
  if (!force && serialWasReady && !stateChanged &&
      static_cast<uint32_t>(now - lastReportMs) < 15000U) {
    return;
  }

  serialWasReady = true;
  lastWifiConnected = wifiConnected;
  lastAddress = address;
  lastReportMs = now;

  if (wifiConnected && address != IPAddress(0, 0, 0, 0)) {
    // A single write keeps the address intact alongside sensor-task messages.
    Serial.printf("\r\n[EFB] Local web server: http://%u.%u.%u.%u/ (same Wi-Fi)\r\n",
                  static_cast<unsigned int>(address[0]),
                  static_cast<unsigned int>(address[1]),
                  static_cast<unsigned int>(address[2]),
                  static_cast<unsigned int>(address[3]));
  } else if (wifiConnected) {
    Serial.println("[EFB] Wi-Fi connected; waiting for a local IP address.");
  } else {
    Serial.println("[EFB] Wi-Fi not connected; no local server address yet.");
  }
}

void setup() {
  Serial.begin(115200);
  // Give native USB a brief chance to connect, but never require a computer
  // or an open Serial Monitor for Wi-Fi, sensing and the web server to start.
  const uint32_t usbWaitStartedAt = millis();
  while (!Serial && millis() - usbWaitStartedAt < 1500UL) {
    delay(10);
  }
  Serial.println();
  Serial.println("Electronic Field Bag V1.7 | ESP32-S3 N16R2 | BME680 SDA=8 SCL=9 | LTR390 SDA=16 SCL=17");
  Serial.println("Serial output: native USB CDC (USB-labelled port).");
  Serial.printf("Detected flash: %lu bytes | PSRAM: %lu bytes\n",
                static_cast<unsigned long>(ESP.getFlashChipSize()),
                static_cast<unsigned long>(ESP.getPsramSize()));
  if (ESP.getPsramSize() == 0) {
    Serial.println("Warning: PSRAM not detected; verify the N16R2 Quad-SPI settings.");
  }

  if (STATUS_LED_PIN >= 0) {
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
  }

  LocalSensor::begin();
  LocalWaterSensor::begin();
  IrrigationAdvisory::begin();
  connectToHomeWiFi();

  LocalSensor::registerRoutes(server);
  LocalWaterSensor::registerRoutes(server);
  server.on("/field-extensions.js", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/javascript; charset=utf-8", fieldExtensionsScript);
  });
  server.on("/api/advisory", HTTP_GET, handleAdvisoryStatus);
  server.on("/api/advisory", HTTP_POST, handleAdvisoryRequest);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/districts", HTTP_GET, handleDistricts);
  server.on("/api/upazilas", HTTP_GET, handleUpazilas);
  server.on("/weather", HTTP_GET, handleWeather);
  server.on("/weather-chart", HTTP_GET, handleWeatherChart);
  server.on("/submit", HTTP_POST, handleSubmit);
  server.on("/download", HTTP_GET, handleDownload);
  server.onNotFound([]() {
  server.send(404, "text/plain", "Not found.");
  });

  server.begin();
  Serial.println("Web server started.");
  reportWebServerAddress(true);
}

void loop() {
  maintainWiFiConnection();
  reportWebServerAddress();
  server.handleClient();
}

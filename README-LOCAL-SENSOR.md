# Electronic Field Bag V1.7 - Local Sensor Data

This is the ESP32-S3 N16R2 version of V1.7. The original V1.0 project is unchanged.

The existing Weather Information section now contains:

1. **Local Sensor Data:** BME680 temperature (C), relative humidity (% RH), local
   absolute barometric pressure (hPa), estimated altitude (feet), and raw gas
   resistance (kOhm), plus an independent LTR390 UV-index estimate (unitless).
   The BME680 replaces the BMP581; both are not required.
2. **Online Weather Data:** existing Open-Meteo readings, historical table and
   hourly chart, unchanged in meaning.

Local readings appear without selecting a District/Upazila. They are measured
at the physical sensors, not automatically at the selected Upazila.
Missing hardware does not disable the existing weather or record workflow.

## Hardware and build

- Target: **WeAct ESP32-S3-WROOM-1 N16R2**, environment `esp32-s3-n16r2`.
- Sensors: Fermion BME680 (DFRobot SEN0375) and LTR390 UV Sensor (C).
  The existing user-selected library constraints are preserved:
  `adafruit/Adafruit BME680 Library@^2.0.6` and
  `adafruit/Adafruit LTR390 Library@^1.1.2`.
- With power off, disconnect the old BMP581. Connect BME680 **VCC to ESP32 3V3**,
  GND to GND, SDA to GPIO8, and SCL to GPIO9. Use the four-pin I2C side; leave
  the separate 3V3 and SPI-side pins unconnected. Do not feed both power pins.
  This wiring uses 3.3V logic; do not apply 5V to ESP32 GPIOs.
- Initialization probes the documented default `0x76`, then alternate `0x77`,
  at 100 kHz with a 50 ms Wire timeout. No BMP581 addresses are probed.
- LTR390 uses **another hardware I2C controller**, not the BME680 pins:

  | LTR390 connector | WeAct ESP32-S3 |
  | --- | --- |
  | VCC | 3V3 |
  | GND | GND |
  | SDA | GPIO16 |
  | SCL | GPIO17 |
  | INT (if present) | Leave unconnected; this firmware polls |

  Wire with power disconnected. Use printed GPIO numbers, not header positions.
  LTR390 is at address `0x53` on controller 1; BME680 remains on controller 0,
  SDA GPIO8 / SCL GPIO9. Both share ground and use 3.3V logic. Keep wires short.
- No camera power-down or flash GPIOs are configured. The optional time-sync
  LED is disabled (`STATUS_LED_PIN = -1`); no built-in LED wiring is assumed.
- 16 MB flash layout and 2 MB Quad-SPI PSRAM are configured. The existing
  sampling task remains on core 1, with the BME680 driver replacing BMP5xx.
- ArduinoJson remains in the dependency list. PlatformIO creates `.pio`; there
  is no need to copy or move build folders.

Build/upload `esp32-s3-n16r2` using the native USB-labelled port, open Serial Monitor at 115200 baud,
and browse to the ESP32's printed HTTP IP address from the same Wi-Fi. The
existing home Wi-Fi credentials are preserved. This is not laptop localhost.
Uploading replaces the firmware and clears old RAM-held logs: download any
needed CSV first. No hardware upload was performed during development.

## Sampling and display

`LocalSensor::begin()` starts a FreeRTOS task (core 1, priority 1, 4096-byte
stack). It owns the BME680 I2C bus, samples about once per second, and retries failed
initialization/reads after five seconds. `LocalSensor::registerRoutes(server)`
adds the read-only `/api/local-sensor` JSON endpoint and the reference-setting
`POST /api/local-sensor/reference` endpoint to the existing server.
Snapshots are copied briefly under a mutex; HTTP formatting never holds the
mutex or directly accesses the sensor. Stale/failed samples become JSON null,
and the UI clears the values rather than displaying fabricated zeros.

`LocalSensor::begin()` also starts `LocalUvSensor::begin()`: a separate task,
mutex, snapshot and I2C controller for LTR390. It does not depend on BME680
initialization succeeding. The existing JSON endpoint includes flat
`uv_sensor_model`, `uv_sensor_ok`, `uv_sensor_status`, `uv_sequence`,
`uv_sample_age_ms`, `uv_raw_counts` and `uv_index_estimate` fields.
UV failure/staleness clears only UV; BME680 failure clears only its readings.
Loss of the ESP32 HTTP connection clears both. Genuine zero UV is valid and
displays `0.0`; failed or stale UV reads display a dash, never a fabricated zero.

### UV estimate and testing

The LTR390 runs in UVS mode at **18x gain / 20-bit resolution**, with a 1-second
measurement period (the conversion itself takes about 400 ms). The conversion
uses **raw counts / 1400**, from the sensitivity specified in the Lite-On V1.2
datasheet linked by Waveshare, for these exact settings and **no optical window**.
This is a configurable source constant, not a performed calibration. Some other
LTR390 documentation uses a different sensitivity (2300); do not mix a factor
from another reference with an unverified gain, integration time or enclosure.
Compare against a calibrated UV meter before claiming measurement accuracy.

For a short outdoor test, keep the UV detector level and facing unobstructed
sky, away from shadows, with no glass or uncharacterized plastic cover. Keep the
exposed breakout dry and supported on a nonconductive surface. Keep BME680
shaded, ventilated and away from ESP32 heat; the two sensors need different
exposure even though they are in one system. Neither board is a weatherproof
outdoor instrument. This estimate is not a medical/sun-safety reference meter.

The driver's I2C transfers are checked for errors/short reads, with a finite
timeout. Initialization retries every five seconds without repeatedly calling
the allocation-owning Adafruit `begin()` method. UV data-ready is checked before
publishing, and stale/failed/saturated samples are unavailable rather than live.

The background task uses a forced measurement with 8x temperature, 4x pressure,
2x humidity oversampling, IIR size 3 and the Adafruit-example gas heater setting
of 320 C for 150 ms. This is the sensor's internal gas-heater target, not the
reported ambient temperature. The finite measurement wait stays in that task;
HTTP handlers never operate the I2C bus. Public library values are cleared before
each read so a no-fields result cannot be republished as a new measurement.

The JSON endpoint adds `sensor_model`, `relative_humidity_pct`,
`gas_resistance_ohms` and `gas_available`. Gas zero/unavailable becomes null and
a dash without hiding valid temperature, humidity or pressure. Failed/stale
samples clear every measurement, including gas. `gas_available` means a fresh,
nonzero raw library result, NOT a heater-stability or calibration guarantee.

The existing WebServer is synchronous: during online weather downloads it
cannot simultaneously answer sensor HTTP requests. The UI pauses its polling
and labels the pause; the background task continues taking readings. A request
from another browser can also delay updates, which triggers a clear unavailable
or stale message. This is not a concurrent HTTP-server redesign.

## Adjusting the altitude reference

Under **Weather Information > Local Sensor Data**, expand **Adjust altitude
reference**. Enter a current nearby QNH / altimeter setting in hPa and select
**Apply reference**, or use **Reset to 1013.25 hPa** for standard pressure altitude.
The accepted range is 800 to 1100 hPa. The active value appears above the control.

This setting changes the altitude estimate only: temperature and measured local
absolute pressure are unchanged. It is not a pressure-sensor offset calibration.
Do not use the sensor's own local pressure as the sea-level reference unless
the sensor is actually at sea level. Refresh a custom reference as weather changes.

The setting is shared by all browsers connected to the ESP32, kept in RAM only,
and resets to 1013.25 hPa on restart. It is not stored in field records or CSV.
The control is a separate form: it works without filling in District, Upazila or
field inputs, and it cannot submit a field record. Editing the input alone does
not change the setting. Apply/reset controls pause during online weather requests.

The POST requires `Content-Type: application/x-www-form-urlencoded`,
`X-EFB-Request: 1`, and exactly one `reference_pressure_hpa` field containing a
finite decimal value in range. Invalid requests leave the active reference intact.
No cross-origin access is enabled. This is a same-origin request safeguard, not
authentication: use the device only on a trusted local network.

If a request times out, the setting may still have been applied. The UI does not
retry POST automatically; check the active reference when polling resumes before
trying again. A missing sensor does not prevent reference updates, but altitude
is unavailable until valid sensor samples return.

## Interpretation / scope

- Temperature is the BME680's sensor temperature; its gas heater, ESP32 heat,
  sun and enclosures can bias it relative to shaded air temperature. Allow it
  to settle and compare with a suitable reference; no invented offset is used.
- Pressure is measured local absolute pressure, not QNH/sea-level pressure.
  This BME680 library returns Pa: divide by 100 for hPa. Accepted sample ranges
  are 300-1100 hPa, -40 to 85 C and 0-100% RH.
- Altitude uses a runtime reference initialized to 1013.25 hPa. It is a
  standard-atmosphere estimate, not GPS or surveyed
  elevation. Changing weather causes drift. Not for navigation or safety use.
- Gas resistance is not an IAQ score, pollutant concentration, CO2 measurement,
  outdoor AQI or safety alarm. Bosch BSEC processing is not included. Library
  2.0.6 does not expose separate gas-valid/heater-stable flags and accepts either
  flag internally; we deliberately make no stability/IAQ claim. No vendor
  library files were patched.
- The subsection is **live display only**. FieldData and the CSV columns remain
  unchanged; these local sensor values are not yet added to saved records.
- Existing online weather retrieval logic, geographical table and field inputs
  are preserved. This addition does not fix unrelated existing TLS/weather API
  issues or change the weather data source.

## Modified and added files

- `src/main.cpp`: subsection CSS/HTML/JS, online-download pause events, sensor
  integration calls, and an optional LED disabled without claiming any GPIO.
- `src/local_sensor.cpp`: sensor task, validation, altitude and JSON endpoint.
- `include/local_sensor.h`: integration API.
- `src/local_uv_sensor.cpp`, `include/local_uv_sensor.h`: separate LTR390 bus,
  task, checked sampling and snapshot API.
- `platformio.ini`: ESP32-S3 N16R2 memory/USB settings and Adafruit dependency.

## Verification

The BME680 replacement builds successfully with Arduino ESP32 2.0.17,
ArduinoJson 7.4.3 and Adafruit BME680 2.0.6. See `README-ESP32-S3.md` for build
figures, tests and upload instructions. `tests/local_sensor_ui_checks.cjs`
executes the actual embedded JavaScript using a mock DOM/network, including
humidity boundaries, gas-unavailable handling, missing/stale readings and
altitude-reference controls. `tests/bme680_driver_checks.cjs` checks source
integration and a mock-adapted numeric helper, not physical measurements.
Hardware sampling, accuracy and live weather require testing on the actual
ESP32-S3. No upload or hardware calibration was performed.

References: [DFRobot SEN0375 wiring](https://wiki.dfrobot.com/sen0375/),
[Adafruit BME680 2.0.6 API](https://github.com/adafruit/Adafruit_BME680/blob/2.0.6/Adafruit_BME680.cpp),
[Waveshare UV Sensor (C)](https://www.waveshare.com/wiki/UV_Sensor_(C)),
[Lite-On LTR390 V1.2 datasheet](https://files.waveshare.com/upload/a/ac/1_LTR-390UV_Final_DS_V1_2.pdf),
[Adafruit LTR390 library](https://github.com/adafruit/Adafruit_LTR390).

# V1.7 — plot information, JSN water level and Groq field review

This update is for the WeAct ESP32-S3 N16R2 project only. V1.0 is unchanged.

## Before uploading

Download any records you want to retain from the running device. The existing CSV
log holds the latest 100 records in RAM: a reset/upload erases those records.
This update does not add permanent cloud CSV storage. Groq is an explanation
service, not a database.

Build/upload the `esp32-s3-n16r2` environment using the board's USB-labelled port.
Open Serial Monitor at 115200 and use the printed local HTTP address on the same
Wi-Fi. Refresh the page after uploading. No upload was performed during editing.

## Field collection

- **Plot Area (m²)** is directly below Plot ID. It is a positive decimal number,
  stored as `double`, limited in software to 10,000,000 m² (1,000 hectares).
  Convert local land units to square metres before entry. Do not type units into
  the numeric box.
- **Soil Type** is directly below Variety. Select one of the 12 USDA soil-texture
  classes or **Unknown**. Unknown is explicitly missing information, not loam.
- Server validation is shared by record submission and Groq review.
- CSV schema version is now **5**. New columns include `plot_area_m2`, `soil_type`,
  `water_level_source`, `jsn_distance_mm`, `jsn_reference_height_mm` and
  `jsn_water_level_mm`. Missing JSN values export as empty numeric cells, not zero.
  Do not append schema-5 rows beneath a schema-4 header without migrating columns.

## JSN-SR04T V3.3

Confirmed wiring: VCC → 3V3, GND → GND, TRIG → GPIO5, ECHO → GPIO6. Use printed
GPIO numbers, not header positions. The module must use **Mode 0 (TRIG/ECHO)**.
Existing BME680 GPIO8/9 and LTR390 GPIO16/17 are unchanged. Power off to rewire.
ESP32-S3 GPIO is not 5 V tolerant; do not later power the module at 5 V and assume
ECHO is safe without verifying its HIGH level and using suitable conversion.

The local page now displays distance and calibrated surface-water depth. The
sensor samples independently about once a second. No echo, measurements below
230 mm or above 5000 mm, and samples older than 3.5 seconds become unavailable.
An invalid measurement is never interpreted as an empty/dry field.

1. Mount the probe vertically, rigidly and with a clear view of the water.
   Keep its face at least 230 mm above the highest expected water surface, plus
   practical mounting margin. Protect the electronics; only the probe is waterproof.
2. Measure the vertical distance from the probe face to your **zero-water plane**
   (the relevant soil surface or tank bottom).
3. Open **Set zero-water reference** and enter that measured height in mm.
   No mounting height is assumed. The reference is RAM-only and shared by all
   viewers; re-enter after restart or moving the probe.
4. Compare the result against a ruler. Depth is `reference height − distance`.
   A negative result is invalid, not clamped to zero. A genuine equal-height
   reading can give zero depth. Millimetre display does not imply mm accuracy.
5. In Field Information, choose **Use current JSN water level** to select the
   sensor as the source. Saving/reviewing rechecks a fresh server-side sample.
   Typing in the water box switches back to manual entry. JSN failure then does
   not prevent an explicitly manual water measurement.

This air-gap measurement cannot measure root-zone moisture or an underground
water table in an open field. Rice AWD needs a separately validated field-water
tube/mounting method. Waves, foam, plants, condensation and soil irregularities
can give misleading echoes. The firmware does not control any pump.

## Groq

Complete the field entries and select **Get Groq Field Review**. This sends a
bounded snapshot to Groq over verified HTTPS: field entries, selected location,
available recent Open-Meteo cache/history, and fresh/null device sensor values.
It does not fetch new weather, save a record or operate hardware. Refresh weather
first if you want it included. The device location is not GPS-verified against
the selected plot; only collect representative measurements at the intended plot.

The current status is deliberately **Review required**, with irrigation minutes
unset. Groq explains observations and missing information; it is not allowed to
choose the firmware status or duration. Crop stage, validated local thresholds,
irrigation method/root-zone or paddy management, pump flow, efficiency and target
net water requirement still need configuring before actionable irrigation rules
can be developed and field-validated. Manual pump state is not live flow feedback.
AI explanations are unverified text and can still be wrong.

Implementation:

- `src/irrigation_advisory.cpp`: a background worker, one queued request, bounded
  HTTPS/response parsing, `openai/gpt-oss-20b` and strict JSON response shape.
- Minimum 60 seconds between requests; bounded transient retries and extra
  backoff after provider rate limits. API/auth/network/clock failures are explicit.
- Sensor sampling and local HTTP handlers remain independent. New weather HTTPS
  transfers are refused temporarily while Groq is busy to avoid two TLS sessions
  competing for memory. Local readings and a valid existing weather cache remain usable.
- GTS Root R4 verifies the provider. A valid device clock is required. If the
  provider changes CA, update `include/groq_root_ca.h`; do not disable TLS checks.
- Changing form inputs invalidates the visible review. A review is a snapshot,
  not continuous monitoring; the UI also expires it after five minutes.

## API key safety

`include/efb_secrets.h` contains the locally configured key and is gitignored.
The empty `include/efb_secrets.example.h` documents the format. No key is served
in HTML/JS, CSV or application Serial logs. Rotate the key that was pasted into
chat, replace it locally and rebuild/upload. Do not share the real header or
built firmware: the key is embedded in firmware and can be extracted. A backend
proxy is recommended before wider deployment. Keep this prototype LAN-only;
the same-origin request header is not user authentication. Do not port-forward it.

Only a synthetic connectivity request was used during development; real field
data is sent only when the review button is used. Free-tier limits and model
availability can change.

## Verification

Run `pio run -e esp32-s3-n16r2`. Node checks in `tests/*checks.cjs` cover the
existing BME680/UV/weather UI plus new field/CSV, JSN status/calibration and Groq
parser/policy/UI behavior. Some tests explicitly adapt C++ helper bodies to JS;
they are not native driver or hardware measurements. A firmware build and a
desktop HTTPS test cannot prove sensor timing, electrical safety, Wi-Fi/TLS
memory availability or water-depth accuracy on the actual ESP32. Test these on
the bench before relying on field measurements.

Sources: [JSN V3.3 manufacturer manual](https://make.net.za/wp-content/datasheets/JSN-SR04T-V3.3%20Datasheet%20-%20Original.pdf),
[HCSR04 library](https://github.com/Martinsos/arduino-lib-hc-sr04),
[Groq API](https://console.groq.com/docs/api-reference),
[Groq limits](https://console.groq.com/docs/rate-limits).

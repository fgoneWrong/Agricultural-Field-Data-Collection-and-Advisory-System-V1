# Hourly rainfall, temperature, humidity and UV chart

Under **Hourly Rainfall and Temperature — Previous 7 Completed Days**, select
**Show humidity & UV index** to reveal a second, time-aligned panel. The original
rainfall bars and temperature line remain above it. Uncheck to collapse it.

- Green: relative humidity, using the left 0–100% axis.
- Amber: hourly UV index, using its own right-hand, unitless scale.
- Tap/hover a time to see values; the tooltip includes humidity and UV when enabled.
- The graph grows taller when the option is enabled, to keep both panels readable.
- The option redraws the cached chart; it makes no new weather API request.

## Data and missing readings

Both the Forecast fallback and Historical Forecast requests include
`hourly=precipitation,temperature_2m,relative_humidity_2m,uv_index`.
UV is the hourly API series, not a repetition of `daily.uv_index_max`.

The ESP32 stores UV in tenths in an `int16_t` field. `/weather-chart` emits the
168-element `uv_index_tenths` array. The browser divides valid integers by ten.
Missing/invalid UV values are JSON nulls and appear as gaps, never invented zeros.
Valid nighttime zero remains zero. Missing UV does not disable rainfall,
temperature or humidity. A note explains missing UV when the extra panel is open.

The selected Upazila reference coordinates, data-source fallback policy and
Bangladesh time (`Asia/Dhaka`, UTC+6) are unchanged. The chart contains the same
seven completed local dates as the table, excluding the current date.
Hourly series remain display-only and are not added to saved records or CSV.

## Comparing another Open-Meteo chart

Use identical requested coordinates, returned grid location, model/source, time
zone and dates. A chart without `timezone=Asia/Dhaka` normally uses GMT/UTC;
late-evening UTC rain belongs to the following date in Bangladesh. A request
with `forecast_days=1` also includes today's full forecast, while this EFB chart
excludes today. A supplied city point and an Upazila reference point can select
different weather grid cells even when both are described as Gazipur.

## Verification and deployment

Tests in the workspace `tests/hourly_uv_checks.cjs` exercise decoding, optional
data, toggle behavior, gap rendering, tooltip units and drawing. Existing local
sensor/altitude-reference tests remain separate. The browser preview uses
explicitly labelled simulated data; it is not a hardware or weather-accuracy test.

Build and upload `esp32-s3-n16r2` through the native USB-labelled port, then refresh the browser. Download
any needed RAM-held CSV log before an upload/restart. Development does not upload
automatically. See `README-LOCAL-SENSOR.md` for the sensor and reference control.

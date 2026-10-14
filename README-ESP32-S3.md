# Electronic Field Bag V1.7 - WeAct ESP32-S3 N16R2

Only **Electronic Field Bag V1.7** is adapted here. **V1.0 remains unchanged**
for the original ESP32-CAM. The previous V1.7 classic-ESP32 configuration,
source files, documentation and VS Code metadata are preserved under
`backups/before-esp32s3-n16r2-20260922-223214`.
The subsequent UART-to-native-USB update is separately backed up under
`backups/before-native-usb-20260922-225414`.
The later Serial Monitor URL-reporting update is backed up under
`backups/before-serial-url-20260923-041305`.
The BMP581-to-BME680 replacement is backed up under
`backups/before-bme680-20260923-093233`.
The separate-bus LTR390 addition is backed up under
`backups/before-ltr390-20260927-195716`.

The provided photo shows ESP32-S3-WROOM-1 with an N16R2 suffix. This variant has
**16 MB Quad-SPI flash and 2 MB Quad-SPI PSRAM**. It is not an N16R8 module:
do not select Octal/OPI PSRAM. Settings follow the photo; actual memory detection
still needs verification on the physical board after upload.

## Initial board check

Keep the previous overheating board disconnected. Test this new S3 board with
only one USB cable attached, with all sensors and other wires disconnected.
If it heats rapidly, smells unusual or causes a USB power-surge warning,
disconnect it and stop testing. Normal startup LEDs alone do not prove the
firmware is running. Use the serial boot log for verification.

The photographed headers are not populated. Use properly soldered headers or
another secure connection, not loose wires pushed into plated holes. Do not
connect the suspect BMP581 until its wiring/supply and solder joints have been
checked with power removed. A software change cannot repair a damaged sensor.

## USB and PlatformIO

- Open this **Electronic Field Bag V1.7** folder in VS Code.
- Select the default environment **esp32-s3-n16r2**. The base board is
  `esp32-s3-devkitc-1`, with 16 MB flash and `qio_qspi` memory overrides.
  PlatformIO may still print the base board's **N8 / 8 MB / No PSRAM** label;
  that generic banner does not describe the overrides. The binary build uses
  `--flash_size 16MB` and the 16 MB partition table; verify detected memory in
  the serial startup log after uploading.
- Use the connector labelled **USB** for both upload and Serial Monitor.
  `ARDUINO_USB_MODE=1` selects Hardware CDC/JTAG, and
  `ARDUINO_USB_CDC_ON_BOOT=1` routes the application's `Serial` output to it.
  The UART-labelled connector will not receive the application's `Serial`
  messages in this build. Do not choose a connector by insert color alone.
- Connect only this board during automatic port detection, or select its COM
  port explicitly. COM6 worked for your native USB upload, but Windows may
  assign a different number after reset/re-enumeration. No COM number is pinned
  in `platformio.ini`. Keep GPIO19/20 free for native USB. The application does
  not drive the addressable RGB LED.
- Dependencies are ArduinoJson, Adafruit BME680 and Adafruit LTR390; PlatformIO manages
  `.pio` automatically. Old classic-ESP32 build artifacts may still be present
  there, but are not selected by the new environment. Do not flash an old binary.

Commands from this project folder:

```powershell
pio run -e esp32-s3-n16r2
pio run -e esp32-s3-n16r2 -t upload
pio device monitor -b 115200
```

Run the upload command only when ready to replace this board's firmware. Do not
upload it to ESP32-CAM or a classic ESP32. Download needed RAM-held CSV records
before restarting any board. The new board does not inherit the old board's logs
or in-RAM altitude reference. No upload is performed automatically during development.

Close Serial Monitor before uploading. If automatic boot entry fails, hold
BOOT, press and release RST, then release BOOT. Wait for the native USB port
to appear, select its current COM number and retry the upload through **USB**.
BOOT recovery is for entering the bootloader; it cannot fix a COM number that
does not exist. If COM6 is missing, select the actual detected port instead.

After upload, keep the same USB connection, open Serial Monitor at **115200 baud**
and press RST if needed. Native USB can disconnect briefly during reset; wait
for it to return and reselect/reopen the port if the monitor does not reconnect.
Expect:

```text
Electronic Field Bag V1.7 | ESP32-S3 N16R2 | BME680 SDA=8 SCL=9 | LTR390 SDA=16 SCL=17
Serial output: native USB CDC (USB-labelled port).
Detected flash: 16777216 bytes | PSRAM: 2097152 bytes
```

The memory line shows expected values, not a hardware test already performed.
If PSRAM is zero or memory differs, save the complete boot log and check the
module/configuration before further changes. Then browse to the printed HTTP
address from a device on the same Wi-Fi; the S3 may get a new IP address.

Startup waits at most 1.5 seconds for native USB before continuing, so sensing,
Wi-Fi and the web server still start without a computer/terminal attached.
Opening a monitor late is not guaranteed to replay earlier boot messages.
The current server URL is now re-announced approximately every **15 seconds**,
so it remains discoverable after opening the monitor late. Reporting starts
after the server is started, and also happens on detected USB reconnection,
Wi-Fi status changes and IP address changes. A synchronous weather request can
delay a loop-based announcement; there is no new blocking delay in the loop.

For example (your DHCP address can differ):

```text
[EFB] Local web server: http://192.168.0.208/ (same Wi-Fi)
```

The address is read from `WiFi.localIP()`, not hard-coded. Disconnected Wi-Fi
produces `Wi-Fi not connected; no local server address yet.`; a connection
without an assigned address produces `waiting for a local IP address.`
BME680 failure messages do not disable this reporting or the web server.

`monitor_dtr=0` and `monitor_rts=0` are retained to avoid monitor-triggered
reset/download-mode control-line sequences.

## BME680 wiring (replaces the BMP581)

Disconnect power before wiring. Use the printed **GPIO numbers**, not header
positions. Disconnect the old BMP581. This mapping is for the Fermion SEN0375
four-pin I2C connector, following the manufacturer's documentation.

| Fermion BME680 breakout | WeAct ESP32-S3 |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |

Leave the separate 3V3 and SPI-side pins unconnected; do not feed both power
pins. Use 3.3V I2C logic; do not put 5V on ESP32 GPIOs. The code probes 0x76 then
0x77 at 100 kHz. The previous GPIO21/22 Dev-board wiring and GPIO13/14 CAM
wiring are not the wiring for this S3 build. A missing sensor is reported as
unavailable without blocking weather retrieval or field-record entry.

## Preserved behavior

LTR390 is connected to a **separate I2C bus**: VCC -> 3V3, GND -> GND,
SDA -> GPIO16, SCL -> GPIO17; INT is unused. Disconnect power before wiring.
The BME680 wiring above does not change. The sixth Local Sensor Data card is
**UV index (estimate)**, with independent status. See `README-LOCAL-SENSOR.md`
for the conversion assumptions and outdoor test placement.

The existing Wi-Fi settings, Bangladesh coordinate table, weather requests,
historical table, humidity/UV chart option, field form, CSV export and
altitude-reference controls are preserved. The local BME680 data remain live
display only; they have not been added to saved CSV records. Local humidity
and raw gas resistance are now displayed alongside temperature and pressure.
The setting labelled altitude reference still changes calculated altitude,
not measured local pressure. No sensor calibration or weather-source change
is part of this sensor replacement.

## Reference information

- [Espressif N16R2 Quad-SPI memory table](https://docs.espressif.com/projects/arduino-esp32/en/latest/troubleshooting.html)
- [ESP32-S3-WROOM-1 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [PlatformIO base board](https://docs.platformio.org/en/latest/boards/espressif32/esp32-s3-devkitc-1.html)
- [Espressif USB/UART Serial selection](https://docs.espressif.com/projects/arduino-esp32/en/latest/troubleshooting.html#serial-not-printing)
- [DFRobot Fermion BME680 SEN0375 wiring](https://wiki.dfrobot.com/sen0375/)

Firmware build and UI regression checks validate software only. Board power,
actual memory, USB upload and BME680 readings require physical testing.

## Initial S3 build verification - 22/09/2026 (before native USB update)

- Build: **SUCCESS** for `esp32-s3-n16r2`, using Espressif32 7.0.1,
  Arduino ESP32 2.0.17, ArduinoJson 7.4.3 and Adafruit BMP5xx 1.0.2.
- Static RAM: **110,420 / 327,680 bytes (33.7%)**, excluding runtime allocation
  and external PSRAM. Application: **1,051,841 / 6,553,600 bytes (16.0%)**.
- The compiled partition table has two 6,400 KiB app slots and ends at 16 MiB.
- Esptool verified the bootloader and application as ESP32-S3 (chip ID 9),
  each with a 16 MB flash header and valid checksums/hashes.
- The existing local-sensor/reference and hourly humidity/UV UI checks passed.
- SHA-256 checks confirmed all 15 checked V1.0 source/config/document/IDE files
  remained unchanged. V1.7's embedded HTML and coordinate table also match
  their pre-S3 backup exactly. The S3 configuration checks passed.
- VS Code metadata was regenerated for the S3 compiler, libraries and Quad-SPI
  PSRAM SDK. The project includes repeatable Node.js checks:

```powershell
node tests/s3_configuration_checks.cjs
node tests/local_sensor_ui_checks.cjs
node tests/hourly_uv_checks.cjs
node tests/serial_url_checks.cjs
node tests/bme680_driver_checks.cjs
```

These checks use Node.js on the computer, not the ESP32. Build and regenerate
PlatformIO's VS Code metadata before the configuration check. No firmware was
uploaded and no physical sensor accuracy/health test was performed.

## Native USB update verification - 22/09/2026

- Build: **SUCCESS**, same `esp32-s3-n16r2` environment and dependency versions.
- Static RAM: **110,652 / 327,680 bytes (33.8%)**. Application:
  **1,048,149 / 6,553,600 bytes (16.0%)**. Runtime/PSRAM use is not represented
  by the static RAM percentage.
- All three project checks passed after refreshing VS Code metadata.
- Linked firmware contains `HWCDC::begin`, `HWCDC::write`, and `Serial`,
  confirming the native hardware USB CDC implementation is included.
- SHA-256 checks confirmed all 15 checked V1.0 source/config/document/IDE files
  are unchanged. V1.7 sensor implementation, coordinate table and embedded UI
  also match their pre-native-USB backup.
- This updated firmware was built, not uploaded. Re-upload through the
  **USB-labelled connector** before expecting application logs on that port.
  The COM number must match the currently detected native USB device.

## Serial URL reporting verification - 23/09/2026

- Build: **SUCCESS** for `esp32-s3-n16r2` with the same dependencies.
- Static RAM: **110,712 / 327,680 bytes (33.8%)**. Application:
  **1,049,969 / 6,553,600 bytes (16.0%)**.
- All four project checks passed. Serial URL checks combine source-integration
  assertions with a mock-adapted reporter state test; they are not physical
  USB/Wi-Fi tests. They cover late monitor connection, reconnects, IP changes,
  unavailable addresses, the reminder interval and timer rollover.
- Read-only review confirmed weather, CSV, UI, sensor and board settings were
  unchanged by this update. All 15 checked V1.0 files remain unchanged.
- Firmware was built, not uploaded. Upload through the **USB-labelled connector**
  and open Serial Monitor at **115200** to use the new URL reporting.

## BME680 replacement verification - 23/09/2026

- Build: **SUCCESS** for `esp32-s3-n16r2`, Arduino ESP32 2.0.17,
  ArduinoJson 7.4.3 and Adafruit BME680 **2.0.6** (pinned).
- Static RAM: **110,824 / 327,680 bytes (33.8%)**. Application:
  **1,058,513 / 6,553,600 bytes (16.2%)**. Image configured for 16 MB flash.
- Local Sensor Data now shows temperature, humidity, local pressure, estimated
  altitude and raw gas resistance. No IAQ/CO2 calculation is included.
- Source/UI checks cover unit conversion, failed/stale samples, nullable gas,
  reference adjustments, weather-chart regression and repeated Serial URLs.
  These are automated software checks, not sensor accuracy or physical tests.
- All five project checks passed after refreshing VS Code metadata. An
  independent read-only review found no actionable issues. Backup comparison
  confirmed weather/CSV/network implementation, online-weather JavaScript and
  the coordinate table unchanged. SHA-256 verified 13 checked V1.0 files unchanged.
- Upload the new firmware through the **USB-labelled connector**, then use
  Serial Monitor at **115200**. Expected sensor log: `BME680 initialized at 0x76.`
  (or `0x77` for the alternate address). Use the printed HTTP URL on the same Wi-Fi.
- Firmware was built, not uploaded. Download needed RAM-held CSV records before
  uploading/resetting. V1.0 is not part of this replacement.

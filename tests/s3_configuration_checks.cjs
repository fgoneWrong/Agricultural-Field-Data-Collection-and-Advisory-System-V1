const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

// Run after the S3 build and PlatformIO VS Code metadata refresh.
const root = path.resolve(process.argv[2] || path.join(__dirname, '..'));
const read = name => fs.readFileSync(path.join(root, name), 'utf8');
const ini = read('platformio.ini');
const main = read('src/main.cpp');
const sensor = read('src/local_sensor.cpp');
const uvSensor = read('src/local_uv_sensor.cpp');
const metadata = read('.vscode/c_cpp_properties.json');

assert.match(ini, /^default_envs\s*=\s*esp32-s3-n16r2\s*$/m);
assert.match(ini, /^\[env:esp32-s3-n16r2\]\s*$/m);
assert.doesNotMatch(ini, /^\[env:(?:esp32dev|esp32cam)\]/m);
assert.match(ini, /^board\s*=\s*esp32-s3-devkitc-1\s*$/m);
assert.match(ini, /^board_upload\.flash_size\s*=\s*16MB\s*$/m);
assert.match(ini, /^board_build\.partitions\s*=\s*default_16MB\.csv\s*$/m);
assert.match(ini, /^board_build\.flash_mode\s*=\s*qio\s*$/m);
assert.match(ini, /^board_build\.arduino\.memory_type\s*=\s*qio_qspi\s*$/m);
assert.match(ini, /-DBOARD_HAS_PSRAM\b/);
assert.match(ini, /-DARDUINO_USB_MODE=1\b/);
assert.match(ini, /-DARDUINO_USB_CDC_ON_BOOT=1\b/);
assert.doesNotMatch(ini, /-DARDUINO_USB_CDC_ON_BOOT=0\b/);
assert.doesNotMatch(ini, /^\s*(?:upload_port|monitor_port)\s*=/m);
assert.doesNotMatch(ini, /qio_opi|opi_opi/);
assert.match(ini, /^monitor_speed\s*=\s*115200\s*$/m);
assert.match(ini, /^monitor_dtr\s*=\s*0\s*$/m);
assert.match(ini, /^monitor_rts\s*=\s*0\s*$/m);

assert.match(sensor, /constexpr int I2C_SDA_PIN = 8;/);
assert.match(sensor, /constexpr int I2C_SCL_PIN = 9;/);
assert.match(sensor, /Wire\.begin\(I2C_SDA_PIN, I2C_SCL_PIN, 100000\)/);
assert.match(uvSensor, /UV_SDA_PIN = 16;/);
assert.match(uvSensor, /UV_SCL_PIN = 17;/);
assert.match(uvSensor, /TwoWire& uvWire = Wire1;/);
assert.match(sensor, /LocalUvSensor::begin\(\)/);
assert.match(sensor, /LocalUvSensor::appendSnapshot\(doc\)/);
assert.doesNotMatch(sensor, /CAMERA_POWER_DOWN_PIN|FLASH_LED_PIN/);
assert.match(main, /#if !defined\(CONFIG_IDF_TARGET_ESP32S3\)/);
assert.match(main, /constexpr int STATUS_LED_PIN = -1;/);
assert.match(main, /if \(STATUS_LED_PIN >= 0\)/);
assert.match(main, /ESP32-S3 N16R2 \| BME680 SDA=8 SCL=9/);
assert.match(main, /ESP\.getFlashChipSize\(\)/);
assert.match(main, /ESP\.getPsramSize\(\)/);
assert.match(main, /while \(!Serial && millis\(\) - usbWaitStartedAt < 1500UL\) \{\s*delay\(10\);\s*\}/);
assert.doesNotMatch(main, /while\s*\(\s*!Serial\s*\)/);
assert.match(main, /Serial output: native USB CDC \(USB-labelled port\)/);

assert.match(metadata, /esp32-s3-n16r2/);
assert.match(metadata, /esp32s3\/qio_qspi\/include/);
assert.match(metadata, /ARDUINO_ESP32S3_DEV/);
assert.match(metadata, /BOARD_HAS_PSRAM/);
assert.match(metadata, /ARDUINO_USB_MODE=1/);
assert.match(metadata, /ARDUINO_USB_CDC_ON_BOOT=1/);
assert.doesNotMatch(metadata, /ARDUINO_USB_CDC_ON_BOOT=0/);
assert.doesNotMatch(metadata, /libdeps\/esp32dev\//);
assert.doesNotMatch(metadata, /libdeps\/esp32cam\//);
assert.match(metadata, /xtensa-esp32s3-elf/);
assert.match(metadata, /Adafruit BME680 Library/);
assert.match(metadata, /Adafruit LTR390 Library/);
assert.doesNotMatch(metadata, /Adafruit BMP5xx Library/);

const build = path.join(root, '.pio/build/esp32-s3-n16r2');
for (const file of ['bootloader.bin', 'partitions.bin', 'firmware.bin']) {
  assert.ok(fs.statSync(path.join(build, file)).size > 0, file + ' must exist');
}
console.log('PASS: S3 target, N16R2 Quad memory settings, native USB Serial, bounded startup wait, unpinned COM ports, I2C pins, disabled LED, target guard, build artifacts and refreshed IntelliSense. Hardware not tested.');

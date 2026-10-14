const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const root = path.resolve(__dirname, '..');
const read = file => fs.readFileSync(path.join(root, file), 'utf8');
const sensor = read('src/local_sensor.cpp');
const ini = read('platformio.ini');
assert.match(ini, /adafruit\/Adafruit BME680 Library@\^?2\.0\.6/);
assert.doesNotMatch(ini, /BMP5xx/);
assert.match(sensor, /#include <Adafruit_BME680\.h>/);
assert.doesNotMatch(sensor, /BMP5XX|Adafruit_BMP5xx|pressureSensor|dataReady\(/);
assert.match(sensor, /addresses\[\] = \{0x76, 0x77\}/);
assert.match(sensor, /environmentalSensor\.begin\(address, false\)/);
for (const setting of ['setTemperatureOversampling', 'setPressureOversampling',
  'setHumidityOversampling', 'setIIRFilterSize', 'setODR', 'setGasHeater']) {
  assert.ok(sensor.includes('!environmentalSensor.' + setting + '('), setting + ' must be checked');
}
assert.match(sensor, /setGasHeater\(320, 150\)/);
assert.match(sensor, /environmentalSensor\.temperature = NAN;\s*environmentalSensor\.humidity = NAN;\s*environmentalSensor\.pressure = 0;\s*environmentalSensor\.gas_resistance = 0;\s*if \(!environmentalSensor\.performReading\(\)\)/);
assert.match(sensor, /const float pressureHpa = environmentalSensor\.pressure \/ 100\.0f;/);
assert.match(sensor, /xTaskCreatePinnedToCore\(samplingTask, "efb-bme680", 4096/);
assert.match(sensor, /SENSOR_RETRY_MS = 5000/);
assert.match(sensor, /FRESH_SAMPLE_MS = 3500/);
const snapshot = sensor.slice(sensor.indexOf('void fillSnapshot'), sensor.indexOf('void sendReferenceError'));
assert.doesNotMatch(snapshot, /environmentalSensor\.|Wire\./, 'HTTP never performs I2C');
for (const field of ['temperature_c', 'station_pressure_hpa', 'relative_humidity_pct',
  'gas_resistance_ohms', 'altitude_ft']) {
  assert.ok(snapshot.includes('doc["' + field + '"] = nullptr;'), field + ' defaults to null');
}
assert.match(snapshot, /const bool fresh = snapshot\.sensorReady && snapshot\.hasSample &&\s*sampleAge <= FRESH_SAMPLE_MS/);
assert.match(snapshot, /if \(fresh\) \{[\s\S]*if \(snapshot\.gasResistanceOhms > 0\) \{/);
assert.match(snapshot, /doc\["gas_available"\] = false/);
// Small, explicit C++-to-JS adaptation of the real numeric validation helper.
// Firmware compilation checks native C++ types; this is not a hardware test.
const body = sensor.match(/bool validReading\([^)]*\) \{([\s\S]*?)\n\}/)[1]
  .replaceAll('std::isfinite', 'Number.isFinite').replace(/(\d+\.\d+)f/g, '$1');
const valid = vm.runInNewContext('(function(pressureHpa, temperatureC, humidityPct) {' + body + '})', {Number});
for (const values of [[300, -40, 0], [1100, 85, 100], [1013.25, 28.4, 63.2]]) assert.equal(valid(...values), true);
for (const values of [[299.9, 25, 50], [1100.1, 25, 50], [1000, -40.1, 50],
  [1000, 85.1, 50], [1000, 25, -1], [1000, 25, 101], [0, NaN, NaN],
  [NaN, 25, 50], [1000, NaN, 50], [1000, 25, NaN], [Infinity, 25, 50]]) assert.equal(valid(...values), false);
console.log('PASS: BME680 source integration, checked configuration, stale-field guards, Pa/hPa conversion, nullable gas, task ownership and mock-adapted numeric validation. Hardware not tested.');

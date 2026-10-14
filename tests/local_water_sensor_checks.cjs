const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const sourcePath = process.argv[2] || path.join(__dirname, '../src/local_water_sensor.cpp');
const source = fs.readFileSync(sourcePath, 'utf8');
function body(name) {
  const match = source.match(new RegExp('(?:bool|float|void) ' + name + '\\([^)]*\\) \\{([\\s\\S]*?)\\n\\}'));
  assert.ok(match, 'Actual firmware helper found: ' + name);
  return match[1];
}
function constant(name) {
  const match = source.match(new RegExp('constexpr [^;=]+ ' + name + ' = ([\\d.]+)f?;'));
  assert.ok(match, 'Actual firmware constant found: ' + name);
  return Number(match[1]);
}

// Execute the actual C++ helper bodies with explicit C++/Arduino syntax adapters.
// This is not a separate reimplementation of the calculation. Number models
// C++ double; Math.fround models float output; >>>0 models unsigned millis age.
// Strict decimal grammar and <=16 characters rule out strtod ERANGE inputs;
// firmware compilation remains responsible for native types and library APIs.
function adapt(text) {
  return text
    .replace(/\/\/[^\n]*/g, '')
    .replaceAll('static_cast<size_t>(ch - \'0\')', "Number(ch)")
    .replaceAll('static_cast<float>(value)', 'Math.fround(value)')
    .replaceAll('std::strtod(text, &end)', '(end = text.length, Number(text))')
    .replaceAll('end != text + length', 'end !== length')
    .replaceAll('std::isfinite', 'Number.isFinite')
    .replaceAll('nullptr', 'null').replaceAll('NAN', 'NaN')
    .replace(/(\d+\.\d+)f\b/g, '$1')
    .replace(/const char\*/g, 'let')
    .replace(/const (?:float|double|char|bool|int|uint32_t|WaterSnapshot|String)\b/g, 'const')
    .replace(/char\* end = null;/g, 'let end = null;')
    .replace(/\b(?:bool|float|size_t|String) (?=[A-Za-z_])/g, 'let ')
    .replace('errno = 0;', 'let errno = 0;')
    .replace('result = Math.fround(value);', 'result.value = Math.fround(value);')
    .replace('const sampleAge = now - snapshot.capturedAt;', 'const sampleAge = (now - snapshot.capturedAt) >>> 0;')
    .replaceAll('text.c_str()', 'text.toString()')
    .replace('parseReferenceHeight(text.toString(), text.length(), referenceHeightMm)',
      'parseReferenceIntoLocal(text.toString(), text.length(), value => { referenceHeightMm = value; })');
}

const sandbox = {
  Number, Math, NaN, ERANGE: 34,
  MIN_DISTANCE_MM: constant('MIN_DISTANCE_MM'),
  MAX_DISTANCE_MM: constant('MAX_DISTANCE_MM'),
  FRESH_SAMPLE_MS: constant('FRESH_SAMPLE_MS'),
  MAX_REFERENCE_TEXT_LENGTH: constant('MAX_REFERENCE_TEXT_LENGTH'),
  MAX_REFERENCE_REQUEST_BYTES: constant('MAX_REFERENCE_REQUEST_BYTES'),
  snapshotMutex: {}, pdTRUE: true, portMAX_DELAY: -1,
  pdMS_TO_TICKS: value => value,
  xSemaphoreTake: () => true,
  xSemaphoreGive: () => {},
  now: 1000,
  sharedSnapshot: null,
  readSnapshot() { return {...sandbox.sharedSnapshot}; },
  millis() { return sandbox.now; },
  sendReferenceError(server, status, error) { server.response = {status, error}; },
  sendSnapshot(server) {
    const doc = {};
    sandbox.appendSnapshot(doc);
    server.response = {status: 200, doc};
  }
};
const context = vm.createContext(sandbox);
for (const [name, args] of [
  ['validDistanceMm', 'distanceMm'], ['waterLevelMm', 'distanceMm, referenceHeightMm'],
  ['parseReferenceHeight', 'text, length, result'], ['publishReading', 'distanceCm, capturedAt'],
  ['appendSnapshot', 'doc'], ['handleReferenceUpdate', 'server']
]) {
  vm.runInContext(`function ${name}(${args}) {${adapt(body(name))}\n}`, context);
}
sandbox.parseReferenceIntoLocal = (text, length, setResult) => {
  const result = {};
  const valid = sandbox.parseReferenceHeight(text, length, result);
  if (valid) setResult(result.value);
  return valid;
};

for (const value of [230, 230.25, 2500.5, 5000]) assert.equal(sandbox.validDistanceMm(value), true);
for (const value of [-1, 0, 229.999, 5000.001, NaN, Infinity, -Infinity]) {
  assert.equal(sandbox.validDistanceMm(value), false, 'Reject distance: ' + value);
}
assert.equal(sandbox.waterLevelMm(400, 1000), 600);
assert.equal(sandbox.waterLevelMm(230, 230), 0, 'A genuine zero depth remains valid');
assert.equal(sandbox.waterLevelMm(230, 5000), 4770);
for (const pair of [[1000, 999], [229, 1000], [5001, 5000], [230, NaN], [NaN, 1000],
  [500, 5001], [500, 0], [Infinity, 1000]]) {
  assert.ok(Number.isNaN(sandbox.waterLevelMm(...pair)), 'Invalid depth stays unavailable: ' + pair);
}

function parse(text, length = text?.length ?? 0) {
  const result = {value: -777};
  const accepted = sandbox.parseReferenceHeight(text, length, result);
  return {accepted, value: result.value};
}
for (const text of ['230', '230.0', '5000', '5000.000', '01000', '1234.567']) {
  assert.deepEqual(parse(text), {accepted: true, value: Math.fround(Number(text))});
}
for (const text of ['', '0', '229.999999', '5000.000001', '-230', '+230', ' 230', '230 ',
  '230\n', '.230', '230.', '2.3e2', '0x100', 'NaN', 'nan', 'Infinity', '1e999',
  '230,5', '230..1', '230\0', '230\0000', '5000.000000000001', '00000000000000230']) {
  assert.deepEqual(parse(text), {accepted: false, value: -777}, 'Reject reference: ' + JSON.stringify(text));
}
assert.equal(parse(null).accepted, false);
assert.equal(parse('2300', 3).accepted, false, 'Reject an end-pointer/length mismatch');

function reset(overrides = {}) {
  sandbox.sharedSnapshot = {
    distanceMm: NaN, referenceHeightMm: NaN, capturedAt: 0, sequence: 0,
    hasSample: false, latestReadingValid: false, status: 'Waiting', ...overrides
  };
  sandbox.now = 1000;
  sandbox.xSemaphoreTake = () => true;
  sandbox.snapshotMutex = {};
}
function snapshot() {
  const doc = {};
  sandbox.appendSnapshot(doc);
  return doc;
}
reset();
assert.equal(snapshot().water_sample_age_ms, null);
assert.equal(snapshot().water_reference_height_mm, null);
assert.equal(snapshot().water_level_valid, false);
sandbox.publishReading(50, 1000);
let doc = snapshot();
assert.equal(doc.water_sensor_ok, true);
assert.equal(doc.water_distance_mm, 500);
assert.equal(doc.water_level_mm, null);
assert.equal(doc.water_level_valid, false);
assert.match(doc.water_sensor_status, /distance only/);
sandbox.sharedSnapshot.referenceHeightMm = 1000;
doc = snapshot();
assert.equal(doc.water_level_mm, 500);
assert.equal(doc.water_level_valid, true);
assert.equal(doc.water_sequence, 1);
sandbox.sharedSnapshot.referenceHeightMm = 400;
doc = snapshot();
assert.equal(doc.water_sensor_ok, true);
assert.equal(doc.water_distance_mm, 500);
assert.equal(doc.water_level_mm, null, 'Negative depth must never be clamped to zero');
assert.equal(doc.water_level_valid, false);
assert.match(doc.water_sensor_status, /exceeds reference/);
sandbox.sharedSnapshot.referenceHeightMm = 1000;
sandbox.now = 4500;
assert.equal(snapshot().water_level_valid, true, '3500ms is still fresh');
sandbox.now = 4501;
doc = snapshot();
assert.equal(doc.water_sensor_ok, false);
assert.equal(doc.water_distance_mm, null);
assert.equal(doc.water_level_mm, null);
assert.equal(doc.water_sample_age_ms, 3501);
assert.equal(doc.water_reference_height_mm, 1000);
assert.match(doc.water_sensor_status, /stale/);

for (const badCm of [-1, 0, 22.9, 500.01, NaN, Infinity]) {
  reset({referenceHeightMm: 1000});
  sandbox.publishReading(50, 1000);
  sandbox.now = 1100;
  sandbox.publishReading(badCm, 1100);
  doc = snapshot();
  assert.equal(doc.water_sensor_ok, false, 'Latest failed reading hides previous good: ' + badCm);
  assert.equal(doc.water_distance_mm, null);
  assert.equal(doc.water_level_mm, null);
  assert.equal(doc.water_level_valid, false);
  assert.equal(doc.water_reference_height_mm, 1000, 'Sampler preserves user reference');
  assert.equal(doc.water_sample_age_ms, 100, 'Failure preserves last-success timestamp');
  assert.equal(doc.water_sequence, 1, 'Failure does not manufacture a successful sample');
  sandbox.publishReading(60, 1100);
  assert.equal(snapshot().water_level_mm, 400, 'Sensor recovers on the next valid reading');
  assert.equal(snapshot().water_sequence, 2);
}
reset({referenceHeightMm: 1000});
sandbox.publishReading(23, 0xfffffff0);
sandbox.now = 20;
assert.equal(snapshot().water_sample_age_ms, 36, 'millis rollover uses unsigned age');
assert.equal(snapshot().water_sensor_ok, true);

// Model only Arduino String methods used in the real POST handler. Property
// access exposes characters as C++ String::operator[] would.
function arduinoString(value) {
  return new Proxy({
    value: String(value),
    indexOf(ch) { return this.value.indexOf(ch); },
    substring(a, b) { return arduinoString(this.value.substring(a, b)); },
    trim() { this.value = this.value.trim(); },
    equalsIgnoreCase(other) { return this.value.toLowerCase() === other.toLowerCase(); },
    length() { return this.value.length; },
    toString() { return this.value; },
    [Symbol.toPrimitive]() { return this.value; }
  }, {get(object, key) { return /^\d+$/.test(String(key)) ? object.value[key] : object[key]; }});
}
function post({headers = {}, args = [['reference_height_mm', '1000']]} = {}) {
  const allHeaders = {
    'X-EFB-Request': '1', 'Content-Type': 'application/x-www-form-urlencoded',
    'Content-Length': '24', ...headers
  };
  const server = {
    header(name) { return arduinoString(allHeaders[name] ?? ''); },
    args() { return args.length; },
    argName(i) { return arduinoString(args[i]?.[0] ?? ''); },
    arg(name) { return arduinoString(args.find(([key]) => key === name)?.[1] ?? ''); }
  };
  sandbox.handleReferenceUpdate(server);
  return server.response;
}
reset();
assert.equal(post().status, 200, 'Reference may be saved while sensor is offline');
assert.equal(snapshot().water_reference_height_mm, 1000);
assert.equal(snapshot().water_sensor_ok, false);
assert.equal(snapshot().water_level_mm, null);
for (const [input, expected] of [
  [{headers: {'X-EFB-Request': ''}}, 403],
  [{headers: {'Content-Type': 'application/json'}}, 415],
  [{headers: {'Content-Length': ''}}, 400],
  [{headers: {'Content-Length': '-1'}}, 400],
  [{headers: {'Content-Length': '0'}}, 413],
  [{headers: {'Content-Length': '97'}}, 413],
  [{headers: {'Content-Length': '1000'}}, 400],
  [{args: []}, 400],
  [{args: [['reference_height_mm', '1000'], ['reference_height_mm', '2000']]}, 400],
  [{args: [['unrelated', '1000']]}, 400],
  [{args: [['reference_height_mm', '229.99']]}, 400],
  [{args: [['reference_height_mm', '5000.000001']]}, 400]
]) {
  assert.equal(post(input).status, expected);
  assert.equal(snapshot().water_reference_height_mm, 1000, 'Rejected POST preserves reference');
}
assert.equal(post({headers: {'Content-Type': 'Application/X-WWW-Form-Urlencoded; charset=UTF-8'}}).status, 200);
sandbox.publishReading(50, 1000);
const prior = snapshot();
assert.equal(post({args: [['reference_height_mm', '1500']]}).doc.water_level_mm, 1000);
assert.equal(snapshot().water_sequence, prior.water_sequence);
assert.equal(snapshot().water_sample_age_ms, prior.water_sample_age_ms);
sandbox.xSemaphoreTake = () => false;
assert.equal(post().status, 503);
assert.equal(snapshot().water_reference_height_mm, 1500);

// Structural ownership checks complement behavioral tests above; they do not
// claim to test GPIO timing, real echoes, FreeRTOS scheduling or native HTTP.
assert.equal(constant('TRIGGER_PIN'), 5);
assert.equal(constant('ECHO_PIN'), 6);
assert.equal(constant('MAX_DISTANCE_CM'), 500);
assert.equal(constant('ECHO_TIMEOUT_US'), 40000);
assert.equal(constant('SAMPLE_INTERVAL_MS'), 1000);
assert.equal(constant('FRESH_SAMPLE_MS'), 3500);
assert.match(body('samplingTask'), /UltraSonicDistanceSensor waterSensor\(TRIGGER_PIN, ECHO_PIN, MAX_DISTANCE_CM,\s*ECHO_TIMEOUT_US\)/);
assert.equal((source.match(/measureDistanceCm\(/g) || []).length, 1);
assert.match(body('samplingTask'), /const float distanceCm = waterSensor\.measureDistanceCm\(\)/);
assert.doesNotMatch(body('begin'), /measureDistanceCm|pinMode|vTaskDelay|delay\(/);
for (const name of ['appendSnapshot', 'sendSnapshot', 'handleReferenceUpdate']) {
  assert.doesNotMatch(body(name), /measureDistanceCm|pinMode|digitalWrite|pulseIn|Wire\.|vTaskDelay/);
}
assert.doesNotMatch(source, /server\.collectHeaders\(/);
assert.match(source, /server\.on\("\/api\/local-water", HTTP_GET/);
assert.match(source, /server\.on\("\/api\/local-water\/reference", HTTP_POST/);
assert.match(source, /xTaskCreatePinnedToCore\(samplingTask, "efb-jsn-sr04t"/);
console.log('PASS: JSN actual-helper adapted math/parser, nullable snapshots, stale/failed/recovered readings, millis rollover, reference POST guards, RAM baseline preservation and task-only hardware ownership. Native build/hardware testing remains separate.');

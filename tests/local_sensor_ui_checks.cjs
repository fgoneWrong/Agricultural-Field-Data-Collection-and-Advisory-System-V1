const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// Pass the deployed main.cpp to test the real project instead of the staging copy.
const sourcePath = process.argv[2] || path.join(__dirname, '../src/main.cpp');
const source = fs.readFileSync(sourcePath, 'utf8');
const html = source.match(/const char htmlPage\[\] PROGMEM = R"HTML\(([\s\S]*?)\)HTML";/)[1];
const scripts = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(m => m[1]);
assert.equal(scripts.length, 3); // two inline scripts plus same-origin field extensions
scripts.forEach(script => new vm.Script(script));
assert.ok(html.indexOf('Weather Information') < html.indexOf('id="localSensorPanel"'));
assert.ok(html.indexOf('id="localSensorPanel"') < html.indexOf('id="weatherPanel"'));

// Form ownership matters: altitude adjustment must not validate or submit field data.
const tags = [...html.matchAll(/<(\/?)([a-zA-Z][\w-]*)\b([^>]*)>/g)];
const elementTags = new Map();
let activeForm = null;
const forms = [];
for (const [, close, tag, attrs] of tags) {
  if (tag === 'form') {
    if (close) {
      assert.ok(activeForm, 'Closing form must have an opener');
      activeForm = null;
    } else {
      assert.equal(activeForm, null, 'Forms must never be nested');
      activeForm = attrs.match(/\bid="([^"]+)"/)?.[1];
      assert.ok(activeForm, 'Each form needs an ID');
      forms.push(activeForm);
    }
  }
  const id = !close && attrs.match(/\bid="([^"]+)"/)?.[1];
  if (id) {
    assert.ok(!elementTags.has(id), 'No duplicate IDs: ' + id);
    elementTags.set(id, {tag, attrs, form: attrs.match(/\bform="([^"]+)"/)?.[1] || activeForm});
  }
}
assert.equal(activeForm, null);
assert.deepEqual(forms, ['fieldForm', 'sensorReferenceForm', 'waterReferenceForm']);
assert.equal(elementTags.get('sensorReferenceInput').form, 'sensorReferenceForm');
assert.equal(elementTags.get('applySensorReference').form, 'sensorReferenceForm');
assert.match(elementTags.get('applySensorReference').attrs, /type="submit"/);
assert.match(elementTags.get('resetSensorReference').attrs, /type="button"/);
for (const id of ['block', 'plotId', 'crop', 'variety', 'water', 'tensiometer', 'saveButton']) {
  assert.equal(elementTags.get(id).form, 'fieldForm', 'Field form ownership: ' + id);
}
assert.match(html, /Resets to 1013\.25 hPa on restart/);
assert.ok(elementTags.has('localSensorUv'), 'The sixth local-sensor card displays UV');
assert.ok(elementTags.has('localSensorUvStatus'), 'UV has its own availability/status text');
assert.ok(html.indexOf('id="localSensorGas"') < html.indexOf('id="localSensorUv"'),
  'UV occupies the card after gas resistance');
assert.match(html, /UV [Ii]ndex \(estimate\)/);

const sample = (overrides = {}) => ({
  ok: true, sensor_ok: true, sensor_status: 'Live', sequence: 1,
  sample_age_ms: 20, temperature_c: 28.4, station_pressure_hpa: 1000,
  relative_humidity_pct: 63.2, gas_available: true, gas_resistance_ohms: 125600,
  altitude_ft: 363.9, reference_pressure_hpa: 1013.25, uptime_ms: 1000,
  uv_sensor_model: 'LTR390', uv_sensor_ok: true, uv_sensor_status: 'Live LTR390 measurement',
  uv_sequence: overrides.sequence ?? 1, uv_sample_age_ms: 20,
  uv_index_estimate: 3.2, uv_raw_counts: 4480,
  ...overrides
});
const flush = () => new Promise(resolve => setImmediate(resolve));
const response = (data, ok = true) => ({ok, json: async () => data});
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return {promise, resolve, reject};
};

function harness() {
  const nodes = new Map();
  for (const [id, {attrs}] of elementTags) {
    const listeners = new Map();
    nodes.set(id, {
      textContent: '', className: '', value: '', disabled: /\bdisabled\b/.test(attrs),
      listeners, validityChecks: 0,
      addEventListener(name, handler) { listeners.set(name, handler); },
      reportValidity() {
        ++this.validityChecks;
        return !!this.value.trim() && Number.isFinite(Number(this.value)) &&
          Number(this.value) >= 800 && Number(this.value) <= 1100;
      }
    });
  }
  const $ = id => { assert.ok(nodes.has(id), 'Missing UI element: ' + id); return nodes.get(id); };
  const timers = new Map(), intervals = [], events = new Map(), calls = [];
  let nextTimer = 0;
  const state = {
    now: 100, payload: sample(), getError: null, postError: null,
    postResponse: null, deferGet: null, deferPost: null
  };
  const window = {
    setTimeout(fn, ms) { const id = ++nextTimer; timers.set(id, {fn, ms}); return id; },
    clearTimeout(id) { timers.delete(id); },
    setInterval(fn) { intervals.push(fn); },
    addEventListener(name, fn) { events.set(name, fn); },
    dispatchEvent(event) { events.get(event.type)?.(event); }
  };
  const context = {
    window, document: {getElementById: $}, performance: {now: () => state.now},
    AbortController, URLSearchParams, Date, Number,
    async fetch(url, options = {}) {
      calls.push({url, options});
      if (options.method === 'POST') {
        if (state.deferPost) {
          const pending = state.deferPost;
          state.deferPost = null;
          return pending.promise;
        }
        if (state.postError) throw state.postError;
        if (state.postResponse) return state.postResponse;
        const reference = Number(new URLSearchParams(options.body).get('reference_pressure_hpa'));
        const pressure = state.payload.station_pressure_hpa;
        state.payload = {
          ...state.payload, reference_pressure_hpa: reference,
          altitude_ft: pressure === null ? null : 44330 * (1 - (pressure / reference) ** 0.1903) / 0.3048
        };
        return response(state.payload);
      }
      if (state.deferGet) {
        const pending = state.deferGet;
        state.deferGet = null;
        return pending.promise;
      }
      if (state.getError) throw state.getError;
      return response(state.payload);
    }
  };
  vm.runInNewContext(scripts[1], context);
  async function tick(data, advance = 1000) {
    if (data !== undefined) state.payload = data;
    state.now += advance;
    const next = [...timers].find(([, timer]) => timer.ms === 1000);
    assert.ok(next, 'A sequential polling timer is scheduled');
    timers.delete(next[0]);
    next[1].fn();
    await flush();
    assert.ok([...timers.values()].filter(timer => timer.ms === 1000).length <= 1,
      'Never more than one polling timer');
  }
  function edit(value) {
    $('sensorReferenceInput').value = value;
    $('sensorReferenceInput').listeners.get('input')();
  }
  function submit() {
    let prevented = false;
    $('sensorReferenceForm').listeners.get('submit')({preventDefault() { prevented = true; }});
    assert.equal(prevented, true, 'Calibration must not navigate/submit the field record');
  }
  return {
    $, state, timers, intervals, calls, tick, edit, submit,
    busy(value) { window.dispatchEvent({type: 'efb-weather-busy', detail: value ? 1 : 0}); },
    reset() { $('resetSensorReference').listeners.get('click')(); }
  };
}

async function liveReadingChecks() {
  const h = harness(); await flush();
  const {$} = h;
  assert.equal($('localSensorTemperature').textContent, '28.4');
  assert.equal($('localSensorHumidity').textContent, '63.2');
  assert.equal($('localSensorGas').textContent, '125.6');
  assert.equal($('localSensorPressure').textContent, '1000.00');
  assert.equal($('localSensorAltitude').textContent, '363.9');
  assert.equal($('localSensorUv').textContent, '3.2');
  assert.equal($('localSensorReference').textContent, '1013.25');
  assert.ok(!$('localSensorStatus').className.includes('error'));
  h.busy(true);
  assert.equal($('localSensorTemperature').textContent, '\u2014');
  assert.equal($('localSensorHumidity').textContent, '\u2014');
  assert.equal($('localSensorGas').textContent, '\u2014');
  assert.equal($('localSensorUv').textContent, '\u2014');
  assert.match($('localSensorUvStatus').textContent, /paused/);
  assert.match($('localSensorStatus').textContent, /paused/);
  for (const id of ['sensorReferenceInput', 'applySensorReference', 'resetSensorReference']) {
    assert.equal($(id).disabled, true, 'Controls disabled while weather is loading');
  }
  const before = h.calls.length;
  await h.tick(); h.submit(); h.reset(); await flush();
  assert.equal(h.calls.length, before, 'No sensor HTTP requests while weather busy');
  h.busy(false);
  await h.tick(sample({sequence: 2}));
  assert.equal($('localSensorPressure').textContent, '1000.00');
  await h.tick(sample({sensor_ok: false, sensor_status: 'BME680 disconnected',
    temperature_c: null, station_pressure_hpa: null, altitude_ft: null}));
  assert.equal($('localSensorPressure').textContent, '\u2014');
  assert.equal($('localSensorStatus').textContent, 'BME680 disconnected');
  assert.equal($('localSensorHumidity').textContent, '\u2014');
  assert.equal($('localSensorGas').textContent, '\u2014');
  await h.tick(sample({sequence: 3, sample_age_ms: 4000}));
  assert.match($('localSensorStatus').textContent, /stale/);
  await h.tick(sample({sequence: 4}));
  await h.tick(undefined, 5100); assert.match($('localSensorStatus').textContent, /stale/);
  await h.tick(sample({sequence: 5, temperature_c: null}));
  assert.equal($('localSensorTemperature').textContent, '\u2014');
  h.state.getError = new Error('offline');
  await h.tick(); assert.match($('localSensorStatus').textContent, /unavailable/);
  assert.equal($('localSensorUv').textContent, '\u2014');
  h.state.getError = null; await h.tick(sample({sequence: 6}));
  assert.equal($('localSensorAltitude').textContent, '363.9');
  assert.equal($('localSensorUv').textContent, '3.2');
  h.state.now += 5100; h.intervals[0]();
  assert.match($('localSensorStatus').textContent, /No recent/);
  assert.equal($('localSensorUv').textContent, '\u2014');
}

async function referenceChecks() {
  const h = harness();
  assert.equal(h.$('applySensorReference').disabled, true, 'Wait for a valid device response');
  await flush();
  const {$} = h;
  assert.equal($('sensorReferenceInput').value, '1013.25');
  assert.equal($('applySensorReference').disabled, false);
  h.edit('1000');
  await h.tick(sample({sequence: 2}));
  assert.equal($('sensorReferenceInput').value, '1000', 'Polling must not overwrite unfinished edits');
  assert.equal($('localSensorReference').textContent, '1013.25', 'Typing does not apply a reference');
  h.submit();
  for (const id of ['sensorReferenceInput', 'applySensorReference', 'resetSensorReference']) {
    assert.equal($(id).disabled, true, 'Controls disabled while saving');
  }
  const duringSave = h.calls.length;
  h.submit(); h.reset();
  assert.equal(h.calls.length, duringSave, 'No duplicate calibration requests');
  await flush();
  const post = h.calls.find(call => call.options.method === 'POST');
  assert.equal(post.url, '/api/local-sensor/reference');
  assert.equal(post.options.headers['Content-Type'], 'application/x-www-form-urlencoded');
  assert.equal(post.options.headers['X-EFB-Request'], '1');
  assert.equal(Number(new URLSearchParams(post.options.body).get('reference_pressure_hpa')), 1000);
  assert.equal([...new URLSearchParams(post.options.body)].length, 1, 'No field-record values in calibration request');
  assert.equal($('localSensorReference').textContent, '1000.00');
  assert.equal($('localSensorAltitude').textContent, '0.0');
  assert.equal($('localSensorPressure').textContent, '1000.00', 'Pressure measurement is unchanged');
  assert.equal($('localSensorTemperature').textContent, '28.4', 'Temperature measurement is unchanged');
  assert.match($('localSensorAltitudeLabel').textContent, /Barometric/);
  assert.match($('sensorReferenceMessage').textContent, /Applied 1000\.00/);
  assert.equal($('sensorReferenceInput').value, '1000.00');
  assert.equal($('applySensorReference').disabled, false);
  assert.equal($('saveButton').disabled, true, 'Reference must not enable field submission');
  assert.equal($('fieldForm').listeners.size, 0, 'Local script must not touch field-form handlers');

  h.edit('');
  const invalidCount = h.calls.length;
  h.submit(); await flush();
  assert.equal(h.calls.length, invalidCount, 'Invalid reference does not POST');
  h.reset(); await flush();
  assert.equal($('localSensorReference').textContent, '1013.25');
  assert.equal($('sensorReferenceInput').value, '1013.25');
  assert.match($('localSensorAltitudeLabel').textContent, /Pressure altitude/);
  assert.equal($('localSensorPressure').textContent, '1000.00');
  assert.equal(h.calls.at(-1).options.body, 'reference_pressure_hpa=1013.25');

  h.edit('8e2'); h.submit(); await flush();
  assert.equal(h.calls.at(-1).options.body, 'reference_pressure_hpa=800.00',
    'Valid HTML exponent notation must become a plain-decimal backend value');
  assert.equal($('localSensorReference').textContent, '800.00');
  h.edit('1100'); h.submit(); await flush();
  assert.equal($('localSensorReference').textContent, '1100.00');
  for (const value of ['799.99', '1100.01', 'NaN', 'Infinity', 'bad']) {
    h.edit(value);
    const requestCount = h.calls.length;
    h.submit(); await flush();
    assert.equal(h.calls.length, requestCount, 'Reject invalid reference: ' + value);
  }
}

async function failedAndOfflineReferenceChecks() {
  const h = harness(); await flush();
  const {$} = h;
  h.edit('1000'); h.state.postError = new Error('offline');
  h.submit(); await flush();
  assert.match($('sensorReferenceMessage').textContent, /Change not confirmed/);
  assert.match($('sensorReferenceMessage').className, /error/);
  assert.equal($('localSensorReference').textContent, '1013.25', 'Network failure is not success');
  assert.equal($('sensorReferenceInput').value, '1000', 'Keep an unconfirmed edit');
  assert.equal($('applySensorReference').disabled, false);
  h.state.postError = null;
  h.state.postResponse = response({ok: false, error: 'Rejected reference'}, false);
  h.submit(); await flush();
  assert.match($('sensorReferenceMessage').textContent, /Rejected reference/);
  assert.equal($('localSensorReference').textContent, '1013.25');
  h.state.postResponse = null;
  await h.tick(sample({sensor_ok: false, sensor_status: 'BME680 disconnected',
    temperature_c: null, station_pressure_hpa: null, altitude_ft: null, sample_age_ms: null}));
  assert.equal($('applySensorReference').disabled, false, 'A disconnected sensor is not an offline ESP32');
  h.submit(); await flush();
  assert.equal($('localSensorReference').textContent, '1000.00');
  assert.equal($('localSensorAltitude').textContent, '\u2014');
  assert.equal($('localSensorStatus').textContent, 'BME680 disconnected');
}

async function interruptedReferenceChecks() {
  for (const weatherInterruption of [false, true]) {
    const h = harness(); await flush();
    const pending = deferred(); h.state.deferPost = pending;
    h.edit('1000'); h.submit();
    const post = h.calls.at(-1);
    const requestsWhileSaving = h.calls.length;
    await h.tick();
    assert.equal(h.calls.length, requestsWhileSaving, 'Polling pauses during calibration');
    if (weatherInterruption) h.busy(true);
    else {
      const timeout = [...h.timers.values()].find(timer => timer.ms === 6000);
      assert.ok(timeout, 'Calibration request has a bounded timeout');
      timeout.fn();
    }
    assert.equal(post.options.signal.aborted, true, 'Pending reference request is cancelled');
    const error = new Error('Cancelled'); error.name = 'AbortError';
    pending.reject(error); await flush();
    assert.equal(h.$('localSensorReference').textContent, '1013.25');
    assert.match(h.$('sensorReferenceMessage').textContent, /Change not confirmed/);
    assert.equal(h.$('sensorReferenceInput').value, '1000');
    assert.equal(h.$('applySensorReference').disabled, weatherInterruption);
    if (weatherInterruption) {
      assert.match(h.$('localSensorStatus').textContent, /paused/);
      h.busy(false); await h.tick(sample({sequence: 2}));
      assert.equal(h.$('applySensorReference').disabled, false);
      assert.equal(h.$('localSensorPressure').textContent, '1000.00');
    }
  }
}

async function staleGetChecks() {
  for (const failure of [false, true]) {
    const h = harness(); await flush();
    const oldGet = deferred(); h.state.deferGet = oldGet;
    await h.tick();
    h.state.payload = sample({uv_sequence: 2, uv_index_estimate: 7.0, uv_raw_counts: 9800});
    h.edit('1000'); h.submit(); await flush();
    assert.equal(h.$('localSensorAltitude').textContent, '0.0');
    if (failure) oldGet.reject(new Error('Old aborted GET failed after POST finished'));
    else oldGet.resolve(response(sample()));
    await flush();
    assert.equal(h.$('localSensorReference').textContent, '1000.00', 'An old GET cannot revert a new reference');
    assert.equal(h.$('localSensorAltitude').textContent, '0.0',
      'An old GET success or error cannot overwrite a successful update');
    assert.equal(h.$('localSensorUv').textContent, '7.0',
      'A superseded GET cannot revert or clear the latest UV snapshot from the POST');
    assert.equal([...h.timers.values()].filter(timer => timer.ms === 1000).length, 1);
  }
}

async function bme680ReadingChecks() {
  const h = harness(); await flush();
  assert.match(html, /Live BME680 and LTR390 measurements/);
  assert.doesNotMatch(html, /BMP581/);
  let sequence = 20;
  for (const humidity of [0, 100]) {
    await h.tick(sample({sequence: sequence++, relative_humidity_pct: humidity}));
    assert.equal(h.$('localSensorHumidity').textContent, humidity.toFixed(1));
  }
  // Gas can be unavailable while all three environmental readings remain live.
  await h.tick(sample({sequence: sequence++, gas_available: false, gas_resistance_ohms: null}));
  assert.equal(h.$('localSensorGas').textContent, '\u2014');
  assert.equal(h.$('localSensorTemperature').textContent, '28.4');
  assert.equal(h.$('localSensorHumidity').textContent, '63.2');
  assert.equal(h.$('localSensorPressure').textContent, '1000.00');
  assert.match(h.$('localSensorGasNote').textContent, /Gas reading unavailable/);
  assert.match(h.$('localSensorGasNote').textContent, /not an IAQ score/);
  for (const invalid of [
    {relative_humidity_pct: null}, {relative_humidity_pct: NaN},
    {relative_humidity_pct: -0.1}, {relative_humidity_pct: 100.1},
    {station_pressure_hpa: 1100.1}, {station_pressure_hpa: 299.9},
    {gas_available: true, gas_resistance_ohms: 0},
    {gas_available: true, gas_resistance_ohms: null},
    {gas_available: true, gas_resistance_ohms: -5},
    {gas_available: 'true'},
  ]) {
    await h.tick(sample({sequence: sequence++, ...invalid}));
    for (const id of ['localSensorTemperature', 'localSensorHumidity', 'localSensorPressure',
      'localSensorAltitude', 'localSensorGas']) assert.equal(h.$(id).textContent, '\u2014');
    assert.equal(h.$('localSensorUv').textContent, '3.2',
      'Malformed BME680 measurements do not invalidate independent UV measurements');
  }
  await h.tick(sample({sequence: sequence++, station_pressure_hpa: 1100}));
  assert.equal(h.$('localSensorPressure').textContent, '1100.00');
  assert.equal(h.$('localSensorGas').textContent, '125.6');
  h.state.now += 5100; h.intervals[0]();
  assert.equal(h.$('localSensorGas').textContent, '\u2014');
  assert.equal(h.$('localSensorHumidity').textContent, '\u2014');
}

async function uvReadingChecks() {
  const h = harness(); await flush();
  const {$} = h;
  assert.equal($('localSensorUv').textContent, '3.2');
  assert.doesNotMatch($('localSensorUvStatus').className, /error/);

  // Darkness is a valid zero measurement, not a disconnected or missing sensor.
  await h.tick(sample({sequence: 2, uv_index_estimate: 0, uv_raw_counts: 0}));
  assert.equal($('localSensorUv').textContent, '0.0');
  assert.doesNotMatch($('localSensorUvStatus').className, /error/);
  await h.tick(sample({sequence: 3, uv_sensor_ok: false,
    uv_sensor_status: 'LTR390 unavailable: check wiring', uv_index_estimate: null,
    uv_raw_counts: null, uv_sample_age_ms: null}));
  assert.equal($('localSensorUv').textContent, '\u2014');
  assert.match($('localSensorUvStatus').textContent, /LTR390 unavailable/);
  assert.equal($('localSensorTemperature').textContent, '28.4');
  assert.equal($('localSensorHumidity').textContent, '63.2');
  assert.equal($('localSensorPressure').textContent, '1000.00');
  assert.equal($('localSensorGas').textContent, '125.6');

  await h.tick(sample({sequence: 4, sensor_ok: false, sensor_status: 'BME680 disconnected',
    temperature_c: null, station_pressure_hpa: null, relative_humidity_pct: null,
    altitude_ft: null, gas_available: false, gas_resistance_ohms: null, sample_age_ms: null,
    uv_index_estimate: 1.6, uv_raw_counts: 2240}));
  assert.equal($('localSensorTemperature').textContent, '\u2014');
  assert.equal($('localSensorUv').textContent, '1.6', 'UV remains live without a BME680');
  assert.doesNotMatch($('localSensorUvStatus').className, /error/);

  let sequence = 5;
  for (const invalid of [
    {uv_index_estimate: null}, {uv_index_estimate: NaN}, {uv_index_estimate: Infinity},
    {uv_index_estimate: '3.2'}, {uv_index_estimate: -0.1},
    {uv_raw_counts: null}, {uv_raw_counts: NaN}, {uv_raw_counts: '4480'},
    {uv_raw_counts: -1}, {uv_raw_counts: 1.5}, {uv_raw_counts: 1048576},
    {uv_sample_age_ms: null}, {uv_sample_age_ms: NaN}, {uv_sample_age_ms: -1},
    {uv_sensor_ok: 'true'}, {uv_sensor_status: null},
    {uv_sequence: null}, {uv_sequence: -1}, {uv_sequence: 1.5},
    {uv_sequence: 4294967296},
  ]) {
    await h.tick(sample({sequence: sequence++, ...invalid}));
    assert.equal($('localSensorUv').textContent, '\u2014',
      'Reject malformed UV fields: ' + Object.keys(invalid).join(','));
    assert.equal($('localSensorTemperature').textContent, '28.4',
      'Malformed UV does not clear independent BME680 readings');
    assert.equal($('localSensorGas').textContent, '125.6');
  }
  for (const invalid of [
    {sensor_ok: 'true'}, {sensor_status: null}, {sequence: -1},
    {sequence: 1.5}, {relative_humidity_pct: null},
  ]) {
    await h.tick(sample({sequence: sequence++, uv_sequence: sequence, ...invalid}));
    assert.equal($('localSensorTemperature').textContent, '\u2014');
    assert.equal($('localSensorUv').textContent, '3.2',
      'Malformed BME680 headers/measurements do not clear UV');
  }
  // Backend handles saturation separately; the raw transport field itself is 20-bit.
  await h.tick(sample({sequence: sequence++, uv_raw_counts: 1048575,
    uv_index_estimate: 1048575 / 1400}));
  assert.equal($('localSensorUv').textContent, (1048575 / 1400).toFixed(1));
  await h.tick(sample({sequence: sequence++}));
  assert.equal($('localSensorUv').textContent, '3.2', 'Good data recover after malformed responses');
}

async function independentSensorStalenessChecks() {
  const h = harness(); await flush();
  const {$} = h;
  await h.tick(sample({sequence: 2, uv_sample_age_ms: 3501}));
  assert.equal($('localSensorUv').textContent, '\u2014');
  assert.match($('localSensorUvStatus').textContent, /stale/);
  assert.equal($('localSensorTemperature').textContent, '28.4');
  await h.tick(sample({sequence: 3, uv_sample_age_ms: 3500}));
  assert.equal($('localSensorUv').textContent, '3.2', 'UV sample-age boundary is inclusive');
  await h.tick(sample({sequence: 4, sample_age_ms: 3501}));
  assert.equal($('localSensorTemperature').textContent, '\u2014');
  assert.equal($('localSensorUv').textContent, '3.2');

  // Fresh HTTP responses and advancing BME sequence must not refresh a frozen UV sample.
  await h.tick(sample({sequence: 5, uv_sequence: 90}));
  for (let step = 1; step <= 5; step++) {
    await h.tick(sample({sequence: 5 + step, uv_sequence: 90}));
  }
  assert.equal($('localSensorUv').textContent, '\u2014');
  assert.match($('localSensorUvStatus').textContent, /stale/);
  assert.equal($('localSensorTemperature').textContent, '28.4');
  await h.tick(sample({sequence: 11, uv_sequence: 91}));
  assert.equal($('localSensorUv').textContent, '3.2');

  // A live UV stream must not make a frozen BME sample appear current either.
  for (let step = 1; step <= 5; step++) {
    await h.tick(sample({sequence: 11, uv_sequence: 91 + step}));
  }
  assert.equal($('localSensorTemperature').textContent, '\u2014');
  assert.match($('localSensorStatus').textContent, /stale/);
  assert.equal($('localSensorUv').textContent, '3.2');

  // Firmware restarts may reuse both counters; decreasing uptime resets both clocks.
  await h.tick(sample({sequence: 11, uv_sequence: 96, uptime_ms: 1}));
  assert.equal($('localSensorTemperature').textContent, '28.4');
  assert.equal($('localSensorUv').textContent, '3.2');
  await h.tick(sample({sequence: 4294967295, uv_sequence: 4294967295, uptime_ms: 100}));
  await h.tick(sample({sequence: 0, uv_sequence: 0, uptime_ms: 200}));
  assert.equal($('localSensorTemperature').textContent, '28.4');
  assert.equal($('localSensorUv').textContent, '3.2', 'Counter rollover remains a new sample');
}

async function uvRequestFailureChecks() {
  const h = harness(); await flush();
  const pending = deferred(); h.state.deferGet = pending;
  await h.tick();
  const request = h.calls.at(-1);
  const timeout = [...h.timers.values()].find(timer => timer.ms === 3500);
  assert.ok(timeout, 'Live sensor requests have a bounded timeout');
  timeout.fn();
  assert.equal(request.options.signal.aborted, true);
  const error = new Error('Timed out'); error.name = 'AbortError';
  pending.reject(error); await flush();
  assert.equal(h.$('localSensorTemperature').textContent, '\u2014');
  assert.equal(h.$('localSensorUv').textContent, '\u2014');
  assert.match(h.$('localSensorUvStatus').textContent, /unavailable/);
  await h.tick(sample({sequence: 2}));
  assert.equal(h.$('localSensorUv').textContent, '3.2');

  const duringWeather = deferred(); h.state.deferGet = duringWeather;
  await h.tick();
  h.busy(true);
  assert.equal(h.calls.at(-1).options.signal.aborted, true);
  duringWeather.resolve(response(sample({sequence: 3}))); await flush();
  assert.equal(h.$('localSensorTemperature').textContent, '\u2014');
  assert.equal(h.$('localSensorUv').textContent, '\u2014',
    'A late in-flight response cannot repopulate UV while weather is busy');
  assert.match(h.$('localSensorUvStatus').textContent, /paused/);
  h.busy(false); await h.tick(sample({sequence: 4}));
  assert.equal(h.$('localSensorUv').textContent, '3.2');
}

async function weatherRequestChecks() {
  // Existing online-weather requests must always balance pause/resume, even on errors.
  const onlineScript = scripts[0];
  const change = onlineScript.slice(onlineScript.indexOf('function changeWeatherTransferCount'), onlineScript.indexOf('function setStatus'));
  const getJson = onlineScript.slice(onlineScript.indexOf('async function getJson'), onlineScript.indexOf('async function loadDistricts'));
  let successful = true;
  const busyEvents = [];
  const onlineCtx = {
    window: {dispatchEvent: event => busyEvents.push(event.detail), setTimeout: () => 1, clearTimeout() {}},
    CustomEvent: class { constructor(type, options) { this.type = type; this.detail = options.detail; } },
    AbortController, fetch: async () => response({ok: successful, error: 'test failure'}, successful)
  };
  vm.runInNewContext('let activeWeatherTransfers=0;\n' + change + getJson, onlineCtx);
  await onlineCtx.getJson('/api/districts'); assert.equal(busyEvents.length, 0);
  await onlineCtx.getJson('/weather?district=test'); assert.deepEqual(busyEvents, [1, 0]);
  successful = false;
  await assert.rejects(onlineCtx.getJson('/weather-chart?district=test'));
  assert.deepEqual(busyEvents, [1, 0, 1, 0]);
}

(async () => {
  await liveReadingChecks();
  await referenceChecks();
  await failedAndOfflineReferenceChecks();
  await interruptedReferenceChecks();
  await staleGetChecks();
  await bme680ReadingChecks();
  await uvReadingChecks();
  await independentSensorStalenessChecks();
  await uvRequestFailureChecks();
  await weatherRequestChecks();
  console.log('PASS: scripts, layout/form ownership, independent BME680/LTR390 live/stale/recovered readings, true zero UV, malformed/null UV isolation, sample-age and frozen-counter checks, reference apply/reset, pressure unchanged, dirty edits, validation, network/server/timeout errors, offline sensors, weather/saving controls, late-GET isolation, balanced weather cleanup.');
})().catch(error => { console.error(error); process.exitCode = 1; });

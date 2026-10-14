const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// Entirely offline: no Wi-Fi, HTTPS, credentials, sensors or paid API calls.
// C++ helper bodies are adapted to JavaScript, with a small ArduinoJson facade.
// This tests real validation/policy logic, not native ABI, TLS or MCU hardware.
const root = path.resolve(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'src/irrigation_advisory.cpp'), 'utf8');
const header = fs.readFileSync(path.join(root, 'include/irrigation_advisory.h'), 'utf8');
const functionBody = name => {
  const match = source.match(new RegExp('\\b' + name + '\\([^)]*\\) \\{([\\s\\S]*?)\\n\\}'));
  assert.ok(match, 'Missing source helper: ' + name);
  return match[1];
};
const constant = name => {
  const match = source.match(new RegExp('constexpr (?:size_t|uint32_t|uint16_t|unsigned) ' + name + ' = ([^;]+);'));
  assert.ok(match, 'Missing constant: ' + name);
  return vm.runInNewContext(match[1]);
};
const constants = Object.fromEntries([
  'MAX_CONTEXT_BYTES', 'MAX_REQUEST_BYTES', 'MAX_RESPONSE_BYTES',
  'MAX_EXPLANATION_BYTES', 'MAX_LIMITATION_BYTES', 'MAX_MODEL_LIMITATIONS',
  'BASE_LIMITATIONS', 'COOLDOWN_MS', 'DEFAULT_RATE_BACKOFF_MS',
  'MAX_RATE_BACKOFF_MS', 'MAX_ATTEMPTS', 'BODY_DEADLINE_MS',
].map(name => [name, constant(name)]));

for (const limit of ['MAX_CONTEXT_BYTES', 'MAX_REQUEST_BYTES', 'MAX_RESPONSE_BYTES']) {
  assert.ok(constants[limit] <= 12 * 1024, limit + ' must remain bounded');
}
assert.equal(constants.COOLDOWN_MS, 60000);
assert.equal(constants.MAX_ATTEMPTS, 2);
assert.match(header, /void begin\(\);/);
assert.match(header, /bool request\(const JsonDocument& context, String& error\);/);
assert.match(header, /void appendStatus\(JsonObject doc\);/);
assert.match(source, /__has_include\("efb_secrets\.h"\)/);
assert.match(source, /#define EFB_GROQ_API_KEY ""/);
assert.match(source, /tls\.setCACert\(EFB_GROQ_ROOT_CA\)/);
assert.doesNotMatch(source, /\.setInsecure\s*\(|Serial\.|printf\s*\(|WebServer|Wire\.|digitalWrite\s*\(/);
assert.match(source, /https:\/\/api\.groq\.com\/openai\/v1\/chat\/completions"/);
assert.doesNotMatch(source, /https:\/\/[^"\s]*\?/);
assert.match(source, /http\.addHeader\("Authorization", authorization\)/);
assert.match(source, /HTTPC_DISABLE_FOLLOW_REDIRECTS/);
assert.match(source, /time\(nullptr\) < MIN_VALID_TIME/);
assert.match(source, /heap_caps_get_free_size/);
assert.match(source, /heap_caps_get_largest_free_block/);
assert.match(source, /if \(!tls\.allocated\(\)\)/);
assert.match(source, /xQueueCreate\(1, sizeof\(AdvisoryJob\*\)\)/);
assert.match(source, /xTaskCreate\(advisoryTask, "efb-groq", 20 \* 1024/);
assert.match(source, /setConnectTimeout\(CONNECT_TIMEOUT_MS\)/);
assert.match(source, /setTimeout\(READ_TIMEOUT_MS\)/);
assert.match(source, /setHandshakeTimeout\(8\)/);
assert.match(source, /http\.writeToStream\(&sink\)/);
assert.doesNotMatch(source, /http\.getString\s*\(/);
assert.match(source, /body\["max_completion_tokens"\] = 600/);
assert.match(source, /body\["reasoning_effort"\] = "low"/);
assert.match(source, /contract\["strict"\] = true/);
assert.match(source, /schema\["additionalProperties"\] = false/);
assert.match(source, /UNTRUSTED DATA/);

const requestBody = functionBody('request');
const pollingBody = functionBody('appendStatus');
assert.doesNotMatch(requestBody + pollingBody, /http\.|\.POST\(|processJob\(|readRegisters\(/);
assert.match(pollingBody, /doc\["status"\] = "review_required"/);
assert.match(pollingBody, /doc\["irrigation_minutes"\] = nullptr/);
assert.equal((source.match(/doc\["irrigation_minutes"\]\s*=/g) || []).length, 1);
assert.equal((source.match(/doc\["status"\]\s*=/g) || []).length, 1);
assert.match(requestBody, /context\.overflowed\(\)/);
assert.match(requestBody, /sharedSnapshot\.state, "busy"/);
assert.match(requestBody, /hasAcceptedRequest \? remainingDelay/);
assert.match(source, /if \(status == 429\) recordRateBackoff/);
assert.match(source, /attempt \+ 1 < MAX_ATTEMPTS && retryableStatus\(status\)/);
assert.doesNotMatch(requestBody.slice(requestBody.indexOf('// Queue ownership')), /job->/,
  'Do not dereference a job after ownership transferred to worker');

const compile = (args, body, extra = {}) => vm.runInNewContext(
  `(function(${args}) {${body}\n})`, {...constants, ...extra});

// request() receives a const JsonDocument. ArduinoJson deliberately rejects
// is<JsonObject>() on a const document, even when its root is an object.
assert.match(requestBody, /context\.is<JsonObjectConst>\(\)/);
assert.doesNotMatch(requestBody, /context\.is<JsonObject>\(\)/);
const contextGuard = requestBody.slice(
  requestBody.indexOf('const size_t bytes = measureJson(context);'),
  requestBody.indexOf('if (xSemaphoreTake(snapshotMutex'));
assert.ok(contextGuard.includes('MAX_CONTEXT_BYTES'), 'Keep the bounded context guard');
const checkContext = compile('context', 'let error = "";\n' + contextGuard
  .replace('const size_t bytes', 'const bytes')
  .replace(/\.is<(Json\w+)>\(\)/g, '.is("$1")')
  .replace(/return false;/g, 'return {accepted:false,error};') +
  '\nreturn {accepted:true,error};', {measureJson: doc => doc.bytes});
const readOnlyContext = (value, options = {}) => ({
  bytes: options.bytes ?? Buffer.byteLength(JSON.stringify(value), 'utf8'),
  overflowed: () => options.overflowed ?? false,
  is: type => type === 'JsonObjectConst' && value !== null &&
    typeof value === 'object' && !Array.isArray(value),
});
const firstTestContext = {
  schema_version: 1,
  field: {water_level_mm: 25, water_level_source: 'manual_spot_measurement'},
  local_device_sensors: {water_sensor_ok: false, water_distance_mm: null, water_level_mm: null},
};
assert.equal(checkContext(readOnlyContext(firstTestContext)).accepted, true,
  'Valid const object with disconnected JSN and manual water must pass');
assert.equal(checkContext(readOnlyContext({})).accepted, true);
for (const value of [null, [], 'text', 12, false]) {
  const result = checkContext(readOnlyContext(value));
  assert.equal(result.accepted, false, 'Reject non-object root');
  assert.match(result.error, /JSON object/);
}
assert.equal(checkContext(readOnlyContext({}, {bytes: constants.MAX_CONTEXT_BYTES})).accepted, true);
const oversized = checkContext(readOnlyContext({}, {bytes: constants.MAX_CONTEXT_BYTES + 1}));
assert.equal(oversized.accepted, false);
assert.match(oversized.error, /size limit/);
const overflow = checkContext(readOnlyContext({}, {overflowed: true}));
assert.equal(overflow.accepted, false);
assert.match(overflow.error, /memory/);
assert.equal(checkContext(readOnlyContext({}, {bytes: 0})).accepted, false);

const retryable = compile('status', functionBody('retryableStatus'));
const statusError = compile('status', functionBody('statusError'));
const backoff = compile('retryAfterSeconds', functionBody('rateBackoffMs')
  .replace(/const uint32_t /g, 'const '));
const remaining = compile('now,startedAt,duration', functionBody('remainingDelay')
  .replace('const uint32_t elapsed = now - startedAt;', 'const elapsed = (now - startedAt) >>> 0;'));
for (const status of [-11, -5, -1, 0, 408, 500, 502, 503, 599]) assert.equal(retryable(status), true);
for (const status of [200, 301, 400, 401, 403, 404, 422, 429, 600]) assert.equal(retryable(status), false);
assert.match(statusError(401), /key/i);
assert.match(statusError(429), /wait/i);
assert.match(statusError(503), /temporarily unavailable/i);
assert.match(statusError(-11), /securely/i);
assert.equal(backoff(0), 120000);
assert.equal(backoff(1), 60000);
assert.equal(backoff(120), 120000);
assert.equal(backoff(999999999), 900000);
assert.equal(remaining(1000, 1000, 60000), 60000);
assert.equal(remaining(60999, 1000, 60000), 1);
assert.equal(remaining(61000, 1000, 60000), 0);
assert.equal(remaining(61001, 1000, 60000), 0);
assert.equal(remaining(20, 0xfffffff0, 60000), 59964, 'millis wrap-around');
assert.equal(remaining(20, 0, 0), 0, 'initial backoff is inactive');

const validTextBody = functionBody('validText')
  .replaceAll('nullptr', 'null')
  .replace('bool visible', 'let visible')
  .replace('size_t i', 'let i')
  .replace('const unsigned char value = static_cast<unsigned char>(text[i]);', 'const value = text[i];');
const validText = compile('text,length,maximum', validTextBody);
for (const text of ['', ' \t\n', 'bad\0text', 'bad\u001btext', '\u007f']) {
  const bytes = Buffer.from(text, 'utf8');
  assert.equal(validText(bytes, bytes.length, 1000), false, JSON.stringify(text));
}
for (const text of ['Review required.', 'Soil\ncontext', 'আর্দ্রতা অজানা']) {
  const bytes = Buffer.from(text, 'utf8');
  assert.equal(validText(bytes, bytes.length, 1000), true);
}

// Minimal ArduinoJson interface for exercising the actual parser/validator.
// Memory allocation behavior is deliberately not modeled; the MCU build checks
// C++ compatibility and runtime allocation failures are checked in the firmware.
function variant(initialValue) {
  let value = initialValue;
  const methods = {
    _set(next) { value = next; },
    isNull() { return value == null; },
    overflowed() { return false; },
    size() { return value == null ? 0 : Object.keys(value).length; },
    is(type) {
      if (type === 'string') return typeof value === 'string';
      if (type === 'JsonArrayConst') return Array.isArray(value);
      if (type === 'JsonObject') return value !== null && typeof value === 'object' && !Array.isArray(value);
      throw new Error('Unknown mock type ' + type);
    },
    as(type) {
      if (type === 'string') return typeof value === 'string' ? value : '';
      if (type === 'JsonString') return {
        c_str: () => Buffer.from(value, 'utf8'),
        size: () => Buffer.byteLength(value, 'utf8'),
      };
      if (type === 'JsonArrayConst' || type === 'JsonObjectConst') return proxy;
      throw new Error('Unknown mock conversion ' + type);
    },
    *[Symbol.iterator]() { for (const item of value) yield variant(item); },
  };
  const proxy = new Proxy(methods, {get(target, key) {
    if (key in target) return target[key];
    return variant(value?.[key]);
  }});
  return proxy;
}
function deserializeJson(target, raw, countOrOptions, options) {
  try {
    const text = Buffer.isBuffer(raw)
      ? raw.subarray(0, typeof countOrOptions === 'number' ? countOrOptions : raw.length).toString('utf8')
      : raw;
    const data = JSON.parse(text);
    const limit = (options || countOrOptions).nestingLimit;
    const depth = item => item && typeof item === 'object'
      ? 1 + Math.max(0, ...Object.values(item).map(depth)) : 0;
    if (depth(data) > limit) return true;
    target._set(data);
    return false;
  } catch { return true; }
}
const adaptJson = body => body
  .replace(/JsonDocument (\w+);/g, 'const $1 = variant(null);')
  .replace(/const (?:JsonString|JsonArrayConst|JsonObjectConst|DeserializationError) /g, 'const ')
  .replace(/for \(JsonVariantConst (\w+) : (\w+)\)/g, 'for (const $1 of $2)')
  .replace(/\.is<const char\*>\(\)/g, '.is("string")')
  .replace(/\.as<(?:const char\*|String)>\(\)/g, '.as("string")')
  .replace(/\.(as|is)<(Json\w+)>\(\)/g, '.$1("$2")')
  .replace(/DeserializationOption::NestingLimit\((\d+)\)/g, '{nestingLimit:$1}');
const validationBody = adaptJson(functionBody('validateAdviceObject'))
  .replace('strlcpy(output.explanation, explanation.c_str(), sizeof(output.explanation));',
    'output.explanation = explanation.c_str().toString("utf8");')
  .replace(/strlcpy\(output.limitations\[output.limitationCount\+\+\], item.as\("string"\),\s*MAX_LIMITATION_BYTES \+ 1\);/,
    'output.limitations[output.limitationCount++] = item.as("string");');
const validateAdviceObject = compile('content,output', validationBody, {validText});
const parseResponse = compile('response,output', adaptJson(functionBody('parseResponse')),
  {variant, deserializeJson, validateAdviceObject});
const envelope = (advice, extra = {}) => JSON.stringify({choices: [{
  finish_reason: 'stop', message: {content: JSON.stringify(advice)}, ...extra,
}]});
const baseline = () => ({explanation: 'Local review required.', limitations: ['Stage missing', 'Threshold missing', 'Flow missing']});
const goodAdvice = {explanation: 'Current observations need a local crop and soil review.', limitations: ['Crop stage is unavailable.']};
const success = baseline();
assert.equal(parseResponse(envelope(goodAdvice), success), true);
assert.equal(success.explanation, goodAdvice.explanation);
assert.equal(success.limitationCount, 4);
assert.deepEqual(success.limitations.slice(0, 3), baseline().limitations);
assert.equal(success.limitations[3], goodAdvice.limitations[0]);
const badAdvice = [null, [], {}, {explanation: 'okay'}, {...goodAdvice, status: 'water_now'},
  {...goodAdvice, irrigation_minutes: 20}, {...goodAdvice, explanation: 7},
  {...goodAdvice, explanation: ' '}, {...goodAdvice, explanation: 'x'.repeat(1001)},
  {...goodAdvice, explanation: 'আ'.repeat(334)}, {...goodAdvice, explanation: 'Hidden\0text'},
  {...goodAdvice, limitations: []}, {...goodAdvice, limitations: 'wrong type'},
  {...goodAdvice, limitations: [5]}, {...goodAdvice, limitations: [null]},
  {...goodAdvice, limitations: ['x'.repeat(241)]}, {...goodAdvice, limitations: Array(6).fill('missing')},
];
for (const advice of badAdvice) {
  const output = baseline();
  assert.equal(parseResponse(envelope(advice), output), false, JSON.stringify(advice).slice(0, 100));
  assert.deepEqual(output, baseline(), 'Rejected response must leave deterministic baseline untouched');
}
for (const text of ['', 'not JSON', '{}', '[]', envelope(goodAdvice, {finish_reason: 'length'}),
  envelope(goodAdvice, {message: {content: '{}', refusal: 'No'}}),
  envelope(goodAdvice, {message: {content: 3}}),
  envelope(goodAdvice, {message: {content: '{bad JSON'}}),
  envelope(goodAdvice, {message: {content: '{}'.repeat(7000)}})]) {
  assert.equal(parseResponse(text, baseline()), false, 'Malformed/truncated/refused envelope rejected');
}

// Exercise the bounded sink's actual write body with a clock and String mock.
const sinkMatch = source.match(/size_t write\(const uint8_t\* bytes, size_t count\) override \{([\s\S]*?)\n  \}/);
assert.ok(sinkMatch);
function makeSink(startedAt = 0) {
  const state = {...constants, startedAt_: startedAt, now: startedAt, failed: false};
  state.millis = () => state.now;
  state.value_ = {buffer: Buffer.alloc(0), length() { return this.buffer.length; },
    concat(bytes, count) { this.buffer = Buffer.concat([this.buffer, bytes.subarray(0, count)]); return true; }};
  const write = vm.runInNewContext('(function(bytes,count) {' + sinkMatch[1]
    .replace('reinterpret_cast<const char*>(bytes)', 'bytes') + '})', state);
  return {state, write};
}
const sink = makeSink();
assert.equal(sink.write(Buffer.alloc(constants.MAX_RESPONSE_BYTES), constants.MAX_RESPONSE_BYTES), constants.MAX_RESPONSE_BYTES);
assert.equal(sink.write(Buffer.from('x'), 1), 0);
assert.equal(sink.state.failed, true);
assert.equal(sink.state.value_.buffer.length, constants.MAX_RESPONSE_BYTES);
const lateSink = makeSink(100);
lateSink.state.now = 100 + constants.BODY_DEADLINE_MS;
assert.equal(lateSink.write(Buffer.from('x'), 1), 0);
assert.equal(lateSink.state.failed, true);
const lowMemorySink = makeSink();
lowMemorySink.state.value_.concat = () => false;
assert.equal(lowMemorySink.write(Buffer.from('x'), 1), 0);
assert.equal(lowMemorySink.state.failed, true);

// Mock a provider status sequence using the firmware's actual retry policy.
function attempts(statuses) {
  let count = 0;
  for (let i = 0; i < constants.MAX_ATTEMPTS; ++i) {
    const status = statuses[i] ?? statuses.at(-1);
    ++count;
    if (status === 200 || !retryable(status)) break;
  }
  return count;
}
assert.equal(attempts([401, 200]), 1);
assert.equal(attempts([429, 200]), 1);
assert.equal(attempts([503, 200]), 2);
assert.equal(attempts([-11, -11, 200]), 2);
console.log('PASS: offline advisory contract, TLS/allocation guards, fixed review gate, mocked actual JSON parser/text validation, capped stream, 401/429/5xx policy, bounded retries, cooldown and millis wrap-around. Hardware/TLS/Groq service not tested.');

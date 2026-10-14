const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(process.argv[2] || path.join(__dirname, '../src/main.cpp'), 'utf8');
const match = source.match(/void reportWebServerAddress\(bool force = false\) \{([\s\S]*?)\n\}\n\nvoid setup\(\)/);
assert.ok(match, 'Reporter must exist before setup');
assert.match(source, /server\.begin\(\);\s*Serial\.println\("Web server started\."\);\s*reportWebServerAddress\(true\);/);
assert.match(source, /void loop\(\) \{\s*maintainWiFiConnection\(\);\s*reportWebServerAddress\(\);\s*server\.handleClient\(\);/);
assert.doesNotMatch(match[1], /delay\(|while\s*\(|sensorReady|sensor_ok|192\.168\./);
assert.equal((match[1].match(/Serial\.printf\(/g) || []).length, 1, 'URL must use one write');
assert.equal((match[1].match(/WiFi\.localIP\(/g) || []).length, 1, 'Sample IP once per report check');

// Execute this small, extracted C++ subset with JS mocks. Keep the adapter
// explicit: firmware compilation separately verifies the actual C++ types.
const statics = [...match[1].matchAll(/^  static .+;$/gm)].map(m => m[0]);
assert.equal(statics.length, 4);
const initializers = statics.join('\n')
  .replace(/static (?:bool|uint32_t) /g, 'let ')
  .replace(/static IPAddress (\w+)\(([^;]+)\);/g, 'let $1 = IPAddress($2);');
let body = match[1].replace(/^  static .+;$/gm, '')
  .replace(/const (?:bool|uint32_t|IPAddress) /g, 'const ')
  .replace('if (!Serial)', 'if (!Serial.ready)')
  .replace(/address != lastAddress/g, '!address.equals(lastAddress)')
  .replace(/address != IPAddress\(0, 0, 0, 0\)/g, '!address.equals(IPAddress(0, 0, 0, 0))')
  .replace('static_cast<uint32_t>(now - lastReportMs)', '((now - lastReportMs) >>> 0)')
  .replace(/static_cast<unsigned int>\((address\[\d\])\)/g, '$1')
  .replace(/15000U\b/g, '15000');
assert.doesNotMatch(body, /static_cast|\bconst (?:bool|uint32_t|IPAddress)\b/);

function harness() {
  const state = {ready: true, connected: true, now: 0, ip: [192, 168, 0, 208], ipReads: 0};
  const lines = [];
  function IPAddress(...octets) {
    const value = [...octets];
    value.equals = other => value.every((part, index) => part === other[index]);
    return value;
  }
  const Serial = {
    get ready() { return state.ready; },
    println(text) { lines.push(text); },
    printf(format, ...args) {
      let index = 0;
      lines.push(format.replace(/%u/g, () => String(args[index++])).trim());
    }
  };
  const context = vm.createContext({Serial, IPAddress, WL_CONNECTED: 3,
    millis: () => state.now >>> 0,
    WiFi: {status: () => state.connected ? 3 : 6,
      localIP: () => { state.ipReads++; return IPAddress(...state.ip); }}
  });
  const report = vm.runInContext(`(() => { ${initializers}\nreturn function(force = false) { ${body}\n}; })()`, context);
  return {state, lines, report};
}

let h = harness();
h.report(true);
assert.deepEqual(h.lines, ['[EFB] Local web server: http://192.168.0.208/ (same Wi-Fi)']);
h.state.now = 14999; h.report(); assert.equal(h.lines.length, 1);
h.state.now = 15000; h.report(); assert.equal(h.lines.length, 2);
h.state.ip = [10, 0, 0, 25]; h.report(); assert.match(h.lines.at(-1), /http:\/\/10\.0\.0\.25\//);
assert.equal(h.lines.length, 3);
h.state.connected = false;
const reads = h.state.ipReads;
h.report(); assert.match(h.lines.at(-1), /Wi-Fi not connected; no local server address yet/);
assert.equal(h.state.ipReads, reads, 'Do not retrieve a stale IP while disconnected');
h.state.connected = true; h.state.ip = [0, 0, 0, 0];
h.report(); assert.match(h.lines.at(-1), /waiting for a local IP address/);
assert.doesNotMatch(h.lines.at(-1), /http:/);
h.state.ip = [10, 0, 0, 25]; h.report(); assert.match(h.lines.at(-1), /10\.0\.0\.25/);
h.state.connected = false; h.report();
h.state.connected = true; h.report(); assert.match(h.lines.at(-1), /10\.0\.0\.25/);
const count = h.lines.length;
h.state.ready = false; h.report(); assert.equal(h.lines.length, count);
h.state.ready = true; h.report(); assert.equal(h.lines.length, count + 1);

h = harness(); h.state.ready = false; h.report(true); assert.equal(h.lines.length, 0);
h.state.ready = true; h.report(); assert.equal(h.lines.length, 1, 'Late monitor attach reports immediately');
h.report(true); assert.equal(h.lines.length, 2, 'Explicit report bypasses interval');
h = harness(); h.state.now = 0xfffffff0; h.report();
h.state.now = (0xfffffff0 + 14999) >>> 0; h.report(); assert.equal(h.lines.length, 1);
h.state.now = (0xfffffff0 + 15000) >>> 0; h.report(); assert.equal(h.lines.length, 2);
h = harness(); h.state.connected = false; h.report();
h.state.now = 15000; h.report(); assert.equal(h.lines.length, 2, 'Unavailable status repeats too');
console.log('PASS: reporter integration/source checks and mock-adapted state logic: initial/late USB, reconnects, DHCP changes, missing/stale IP, 15-second interval and timer rollover. Hardware not tested.');

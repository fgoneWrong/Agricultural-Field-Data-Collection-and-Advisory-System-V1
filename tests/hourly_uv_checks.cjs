const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

// Optional argument: the deployed main.cpp; default: this staging project's source.
const sourcePath = process.argv[2] || path.join(__dirname, '../src/main.cpp');
const source = fs.readFileSync(sourcePath, 'utf8');
const html = source.match(/const char htmlPage\[\] PROGMEM = R"HTML\(([\s\S]*?)\)HTML";/)?.[1];
assert.ok(html, 'Embedded HTML must exist');
const scripts = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)].map(match => match[1]);
scripts.forEach(script => new vm.Script(script));
const script = scripts.find(value => value.includes('function decodeUv'));
assert.ok(script, 'Chart script must exist');
const chartCode = script.slice(script.indexOf('function chartLayout'), script.indexOf('async function loadWeather'));
const dateCode = script.slice(script.indexOf('function formatRegionalDateObject'), script.indexOf('function renderWeather'));
const toggleCode = script.match(/humidityToggle\.addEventListener\('change', function\(\) \{[\s\S]*?\n    \}\);/)?.[0];
assert.ok(toggleCode, 'Combined-series checkbox needs a change handler');

const section = html.match(/<section id="hourlyChartPanel"[\s\S]*?<\/section>/)?.[0];
assert.ok(section);
assert.match(section, /Hourly Rainfall and Temperature &mdash; Previous 7 Completed Days/);
assert.ok(section.indexOf('Previous 7 Completed Days') < section.indexOf('id="humidityToggle"'));
assert.match(section, /Show humidity &amp; UV index/);
assert.match(section, /id="humidityToggle" aria-controls="hourlyChart humidityLegend uvLegend"/);
assert.match(section, /id="humidityLegend" hidden/);
assert.match(section, /id="uvLegend" hidden/);
assert.match(html, /\.chart-wrap\.show-extra-series #hourlyChart \{ height: 400px; \}/);
assert.match(html, /\.chart-wrap\.show-extra-series #hourlyChart \{ height: 375px; \}/);

function makeNode() {
  const classes = new Set();
  return {
    hidden: false, checked: false, disabled: false, textContent: '', style: {}, attributes: {},
    listeners: new Map(),
    setAttribute(name, value) { this.attributes[name] = value; },
    addEventListener(name, callback) { this.listeners.set(name, callback); },
    classList: {
      toggle(name, value) { if (value) classes.add(name); else classes.delete(name); },
      contains(name) { return classes.has(name); }
    }
  };
}

function makeCanvasContext() {
  const draws = [], texts = [], paths = [];
  let currentPath = [];
  const check = (name, args) => {
    args.forEach(value => assert.ok(Number.isFinite(value), name + ' received non-finite coordinate: ' + value));
    draws.push({name, args});
  };
  return {
    draws, texts, paths,
    setTransform(...args) { check('setTransform', args); },
    clearRect(...args) { check('clearRect', args); },
    fillRect(...args) { check('fillRect', args); },
    beginPath() { currentPath = []; },
    moveTo(...args) { check('moveTo', args); currentPath.push({type: 'M', args}); },
    lineTo(...args) { check('lineTo', args); currentPath.push({type: 'L', args}); },
    arc(...args) { check('arc', args); currentPath.push({type: 'A', args}); },
    stroke() { paths.push({color: this.strokeStyle, points: currentPath.slice()}); },
    fill() {},
    setLineDash(args) { check('setLineDash', args); },
    fillText(text, ...args) { check('fillText', args); texts.push({text, args}); }
  };
}

const sample = (overrides = {}) => ({
  ok: true, sample_count: 168, start_date: '2026-09-13', chart_fetched_at: '20/09/2026 10:00',
  rainfall_tenths_mm: Array.from({length: 168}, (_, i) => i % 12 === 0 ? 6 : 0),
  temperature_tenths_c: Array.from({length: 168}, (_, i) => 250 + i % 24 * 3),
  relative_humidity_percent: Array.from({length: 168}, (_, i) => 75 + i % 21),
  uv_index_tenths: Array.from({length: 168}, (_, i) => i % 24 < 6 || i % 24 > 18 ? 0 : (i % 24 - 6) * 6),
  grid_latitude: 24.01, grid_longitude: 90.38,
  history_source: 'Open-Meteo Historical Forecast API', history_data_type: 'Model estimate',
  ...overrides
});

function harness(payload = sample(), width = 580, height = 400) {
  const context2d = makeCanvasContext();
  const canvas = {...makeNode(), clientWidth: width, clientHeight: height, width: 0, height: 0,
    getContext: () => context2d};
  const nodes = {
    hourlyChartCanvas: canvas, hourlyChartPanel: makeNode(), hourlyChartStatus: makeNode(),
    hourlyChartTooltip: makeNode(), hourlyChartMeta: makeNode(), humidityToggle: makeNode(),
    humidityLegend: makeNode(), uvLegend: makeNode(), hourlyChartUvNote: makeNode(), chartWrap: makeNode()
  };
  const frames = [], requests = [];
  const state = {payload, error: null};
  const context = vm.createContext({
    ...nodes, AbortController, URLSearchParams,
    window: {devicePixelRatio: 3, requestAnimationFrame: callback => frames.push(callback)},
    async getJson(url) { requests.push(url); if (state.error) throw state.error; return state.payload; },
    setHourlyChartStatus(message, type) {
      nodes.hourlyChartStatus.textContent = message;
      nodes.hourlyChartStatus.className = type;
    }
  });
  vm.runInContext(`
    let hourlyChartData = null, selectedHourlyChartIndex = -1, hourlyChartDrawQueued = false;
    let hourlyChartRequestId = 0, hourlyChartRequestController = null;
    ${dateCode}\n${chartCode}\n${toggleCode}
    globalThis.getData = () => hourlyChartData;
    globalThis.selectIndex = value => { selectedHourlyChartIndex = value; };
  `, context);
  return {
    ...nodes, context2d, state, requests, frames, api: context,
    async load() { await context.loadHourlyChart('Gazipur', 'Gazipur Sadar'); this.flush(); },
    flush() { while (frames.length) frames.shift()(); },
    toggle(value) { nodes.humidityToggle.checked = value; nodes.humidityToggle.listeners.get('change')(); this.flush(); }
  };
}

async function validDataChecks() {
  const h = harness();
  await h.load();
  const data = h.api.getData();
  assert.equal(data.uv.length, 168);
  assert.equal(data.uv[0], 0, 'Zero nighttime UV is a valid value, not missing');
  assert.equal(data.uv[12], 3.6);
  assert.equal(data.rainfall[0], 0.6);
  assert.equal(data.temperature[12], 28.6);
  assert.equal(data.humidity[12], 87);
  assert.equal(h.chartWrap.hidden, false);
  assert.equal(h.humidityToggle.disabled, false);
  assert.equal(h.humidityLegend.hidden, true);
  assert.equal(h.uvLegend.hidden, true);
  assert.equal(h.hourlyChartUvNote.hidden, true);
  assert.doesNotMatch(h.api.hourlyDetailText(12), /Humidity|UV index/);
  assert.equal(h.hourlyChartCanvas.width, 1160, 'Pixel ratio is capped at 2');
  const requestCount = h.requests.length;
  h.toggle(true);
  assert.equal(h.requests.length, requestCount, 'Checkbox must reuse data without network requests');
  assert.equal(h.chartWrap.classList.contains('show-extra-series'), true);
  assert.equal(h.humidityLegend.hidden, false);
  assert.equal(h.uvLegend.hidden, false);
  assert.equal(h.hourlyChartUvNote.hidden, false);
  assert.match(h.hourlyChartUvNote.textContent, /humidity \(%\).*UV index \(unitless\)/);
  assert.match(h.hourlyChartCanvas.attributes['aria-label'], /relative humidity and UV index/);
  assert.match(h.api.hourlyDetailText(12), /13\/09\/2026 12:00 Bangladesh time/);
  assert.match(h.api.hourlyDetailText(12), /Humidity: 87 %\nUV index: 3\.6$/);
  assert.match(h.api.hourlyDetailText(0), /UV index: 0\.0$/);
  assert.ok(h.context2d.paths.some(value => value.color === '#149c68'));
  assert.ok(h.context2d.paths.some(value => value.color === '#b76a00'));
  assert.ok(h.context2d.texts.some(value => value.text === '100%'));
  assert.ok(h.context2d.texts.some(value => value.text === 'UV index'));
  h.api.selectIndex(12); h.api.drawHourlyChart();
  assert.equal(h.hourlyChartTooltip.hidden, false);
  assert.match(h.hourlyChartTooltip.textContent, /UV index: 3\.6$/);
  h.toggle(false);
  assert.equal(h.requests.length, requestCount);
  assert.equal(h.uvLegend.hidden, true);
  assert.equal(h.humidityLegend.hidden, true);
  assert.equal(h.hourlyChartUvNote.hidden, true);
  assert.equal(h.chartWrap.classList.contains('show-extra-series'), false);
  assert.doesNotMatch(h.hourlyChartTooltip.textContent, /UV index|Humidity/);
}

async function missingAndMalformedChecks() {
  for (const value of [undefined, null, 'bad', {}, [], Array(167).fill(0), Array(169).fill(0)]) {
    const h = harness(sample({uv_index_tenths: value}));
    await h.load(); h.toggle(true);
    assert.equal(h.chartWrap.hidden, false, 'Missing optional UV must not hide the base chart');
    assert.equal(h.humidityToggle.disabled, false);
    assert.ok(h.api.getData().uv.every(Number.isNaN));
    assert.match(h.hourlyChartUvNote.textContent, /Hourly UV index is unavailable/);
    assert.match(h.api.hourlyDetailText(12), /UV index: not available$/);
    assert.ok(h.context2d.paths.some(item => item.color === '#6548c9'), 'Temperature still draws');
    assert.ok(h.context2d.paths.some(item => item.color === '#149c68'), 'Humidity still draws');
    assert.ok(!h.context2d.paths.some(item => item.color === '#b76a00'), 'No fabricated zero UV line');
  }
  const uv = sample().uv_index_tenths;
  const invalid = [null, -1, '60', false, true, {}, 1.5, 32768, undefined, Infinity, NaN];
  invalid.forEach((value, i) => uv[i] = value);
  uv[12] = 32767;
  const h = harness(sample({uv_index_tenths: uv}));
  await h.load(); h.toggle(true);
  invalid.forEach((_, i) => assert.ok(Number.isNaN(h.api.getData().uv[i])));
  assert.equal(h.api.getData().uv[12], 3276.7, 'Largest valid packed integer decodes without overflow');
  assert.match(h.hourlyChartUvNote.textContent, /11 hours; gaps are not zero UV/);
  assert.match(h.api.hourlyDetailText(0), /UV index: not available$/);
  h.state.error = new Error('offline');
  await h.load();
  assert.equal(h.chartWrap.hidden, true);
  assert.equal(h.humidityToggle.disabled, true);
  assert.equal(h.uvLegend.hidden, true);
  assert.equal(h.humidityLegend.hidden, true);
  assert.equal(h.hourlyChartUvNote.hidden, true);
  assert.match(h.hourlyChartStatus.textContent, /Hourly chart unavailable/);
}

function gapChecks() {
  const h = harness();
  for (const values of [[1, 2, NaN, 3, 4], [1, 2, NaN], [NaN, 1, 2, NaN, 3, 4, NaN]]) {
    const ctx = makeCanvasContext();
    h.api.drawSeries(ctx, values, 'test', 0, 5, 10, 80, {left: 20, width: 100});
    assert.equal(ctx.paths.length, 1, 'Keep earlier segments even when final sample is missing');
    const expected = [];
    let previousWasFinite = false;
    values.forEach(value => {
      if (!Number.isFinite(value)) { previousWasFinite = false; return; }
      expected.push(previousWasFinite ? 'L' : 'M'); previousWasFinite = true;
    });
    assert.deepEqual(ctx.paths[0].points.map(item => item.type), expected, 'Gap must start a new subpath, not bridge missing UV');
  }
  const empty = makeCanvasContext();
  h.api.drawSeries(empty, [NaN, NaN], 'test', 0, 5, 10, 80, {left: 20, width: 100});
  assert.equal(empty.paths.length, 0);
}

async function layoutChecks() {
  for (const [width, height] of [[280, 375], [320, 375], [580, 400]]) {
    const h = harness(sample(), width, height);
    await h.load(); h.toggle(true);
    const layout = h.api.chartLayout(width, height, true);
    assert.ok(layout.left > 0 && layout.right < width && layout.width > 0);
    assert.ok(layout.top < layout.mainBottom);
    assert.ok(layout.mainBottom < layout.humidityTop);
    assert.ok(layout.humidityTop < layout.humidityBottom);
    assert.ok(layout.humidityBottom < height);
    for (const index of [0, 12, 167]) {
      h.api.selectIndex(index); h.api.drawHourlyChart();
      assert.ok(Number.parseFloat(h.hourlyChartTooltip.style.left) >= 0);
      assert.ok(Number.parseFloat(h.hourlyChartTooltip.style.left) < width);
    }
  }
}

(async () => {
  await validDataChecks();
  await missingAndMalformedChecks();
  gapChecks();
  await layoutChecks();
  console.log('PASS: chart scripts, combined option placement, 168-hour UV decoding including nighttime zero, optional/malformed UV resilience, legends and unitless tooltips, no checkbox refetch, humidity/UV axes, missing-value line gaps, trailing-gap paths, phone/desktop layouts and finite canvas coordinates.');
})().catch(error => { console.error(error); process.exitCode = 1; });

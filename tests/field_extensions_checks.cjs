const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const main = fs.readFileSync(path.join(root, 'src/main.cpp'), 'utf8');
const extension = fs.readFileSync(path.join(root, 'include/field_extensions_ui.h'), 'utf8');
const html = main.match(/R"HTML\(([\s\S]*?)\)HTML";/)[1];
const script = extension.match(/R"JS\(([\s\S]*?)\)JS";/)[1];
new vm.Script(script);
assert.ok(html.includes('<title>Agro-advisory field Toolkit</title>'));
assert.ok(html.includes('<h1>Agro-advisory field Toolkit</h1>'));
assert.ok(html.includes('<p class="intro">IoT-Based Agricultural Data Collection and advisory System</p>'));
assert.doesNotMatch(html, /Electronic Field Bag|plotAreaHelp|Enter a positive decimal area/);
assert.equal((main.match(/<title>Agro-advisory field Toolkit<\/title>/g) || []).length, 2,
  'Home and saved-record browser titles use the requested name');
assert.ok(html.indexOf('id="plotId"') < html.indexOf('id="plotArea"'));
assert.ok(html.indexOf('id="plotArea"') < html.indexOf('id="crop"'));
assert.ok(html.indexOf('id="variety"') < html.indexOf('id="soilType"'));
assert.ok(html.indexOf('id="soilType"') < html.indexOf('id="water"'));
assert.match(main, /double plotAreaM2;/);
assert.match(main, /value <= 0\.0 \|\| value > MAX_PLOT_AREA_M2/);
assert.match(main, /for \(const char\* option : SOIL_TYPE_OPTIONS\)/);
assert.match(main, /if \(!parseManualFieldInputs\(record, error\)\)/);
assert.match(main, /record\.irrigationWaterLevelMm = record\.waterSensorLevelMm/);
assert.match(main, /IrrigationAdvisory::busy\(\)/);
assert.match(main, /LocalSensor::appendSnapshot\(local\)/);
assert.match(main, /LocalWaterSensor::appendSnapshot\(waterSnapshot.to<JsonObject>\(\)\)/);
assert.match(main, /for \(JsonPairConst item : waterSnapshot.as<JsonObjectConst>\(\)\)/);
assert.match(main, /cachedUpazilaIndex == locationIndex/);
assert.match(main, /policy\["irrigation_minutes"\] = nullptr/);
assert.match(main, /policy\["pump_control_enabled"\] = false/);
assert.doesNotMatch(html + script, /gsk_[A-Za-z0-9]+|Authorization|EFB_GROQ_API_KEY/);
assert.doesNotMatch(script, /innerHTML|eval\(/);

// Exercise the actual frontend area-validation function.
const validationBody = main.match(/function validatePlotArea\(\) \{([\s\S]*?)\n    \}/)[1];
const input = {value: '', message: '', setCustomValidity(t) { this.message = t; }};
const validArea = vm.runInNewContext('(function(){' + validationBody + '})', {plotAreaInput: input, Number});
for (const text of ['1','0.001','150.75','10000000','.5','12.']) { input.value = text; assert.equal(validArea(), true, text); }
for (const text of ['', '0', '-1', '10000001', 'NaN', 'Infinity', '1e3', '0x20', '12 m2', '1.2.3']) {
  input.value = text; assert.equal(validArea(), false, text);
}
const prefix = main.match(/String buildCsvHeader\(\) \{[\s\S]*?String header =([\s\S]*?);/)[1];
const columns = [...prefix.matchAll(/"([^"]*)"/g)].map(m => m[1]).join('').split(',');
const rowBody = main.match(/String buildCsvRow\([^)]*\) \{([\s\S]*?)\n  for \(/)[1];
const values = [...rowBody.matchAll(/appendCsv\w+\(row, (.*)\);/g)].map(m => m[1]);
assert.equal(columns.length, values.length, 'CSV base header/row field counts');
for (const [column, expr] of [['plot_area_m2','record.plotAreaM2'],['soil_type','record.soilType'],
  ['jsn_distance_mm','record.waterSensorDistanceMm'],['jsn_reference_height_mm','record.waterReferenceHeightMm'],
  ['jsn_water_level_mm','record.waterSensorLevelMm']]) assert.equal(values[columns.indexOf(column)], expr, column);
assert.match(values[columns.indexOf('water_level_source')], /record.waterFromSensor/);
assert.equal(values[0], '"5"');
assert.match(main, /if \(!resolveWaterSource\(record, error\)\)[\s\S]*?record\.recordId = nextRecordId\+\+/);

const flush = () => new Promise(resolve => setImmediate(resolve));
const defaultWater = () => ({ok:true, uptime_ms:1000, water_sensor_ok:true, water_sensor_status:'Live',
  water_sequence:1, water_sample_age_ms:10, water_distance_mm:400, water_reference_height_mm:null,
  water_level_mm:null, water_level_valid:false});
function harness() {
  const nodes = new Map();
  for (const match of html.matchAll(/\bid="([^"]+)"/g)) nodes.set(match[1], {
    id:match[1], value:'', disabled:false, textContent:'', className:'', children:[], listeners:new Map(),
    addEventListener(name, fn) { this.listeners.set(name, fn); }, reportValidity() {return true;},
    replaceChildren() {this.children=[];}, appendChild(child) {this.children.push(child);}
  });
  const $ = id => {assert.ok(nodes.has(id), id); return nodes.get(id);};
  for (const id of ['water','plotId','plotArea','crop','variety','soilType']) $(id).form=$('fieldForm');
  $('waterReferenceInput').form=$('waterReferenceForm'); $('waterSource').value='manual';
  const state = {now:100, water:defaultWater(), getError:false, adviceError:false, adviceCalls:0};
  const intervals=[], timers=[], events=new Map();
  const window = {setInterval(fn, ms) {intervals.push({fn,ms});},
    setTimeout(fn, ms) {const t={fn,ms,cleared:false}; timers.push(t); return t;},
    clearTimeout(t) {t.cleared=true;}, addEventListener(name,fn) {events.set(name,fn);}};
  const document = {hidden:false, getElementById:$, createElement() {return {textContent:''};}};
  async function fetch(url, options={}) {
    if (url.startsWith('/api/local-water')) {
      if (state.getError) throw new Error('Disconnected');
      if (options.method==='POST') {
        const ref=Number(new URLSearchParams(options.body).get('reference_height_mm'));
        state.water={...state.water, water_reference_height_mm:ref, water_level_valid:ref>=400,
          water_level_mm:ref>=400 ? ref-400 : null};
      }
      return {ok:true, json:async()=>structuredClone(state.water)};
    }
    assert.equal(url,'/api/advisory');
    if (options.method==='POST') {++state.adviceCalls; return {ok:true,json:async()=>({ok:true,state:'busy',request_id:1})};}
    if (state.adviceError) return {ok:true,json:async()=>({ok:false,state:'error',error:'Groq rate limited'})};
    return {ok:true,json:async()=>({ok:true,state:'complete',request_id:1,status:'review_required',irrigation_minutes:null,
      explanation:'<script>untrusted text</script>',limitations:['Missing crop stage']})};
  }
  class FormData { constructor() {this.items=[['water',$('water').value],['water_source',$('waterSource').value]];} [Symbol.iterator](){return this.items[Symbol.iterator]();} }
  vm.runInNewContext(script, {document,window,fetch,performance:{now:()=>state.now},Number,Date,URLSearchParams,
    FormData,AbortController,Promise,Error});
  return {$,state,intervals,timers,events,
    fire(id,name='click') { return $(id).listeners.get(name)({preventDefault(){},target:$(id)}); },
    async poll() { await intervals.find(t=>t.ms===1500).fn(); await flush(); },
    async nextAdvicePoll() {const t=timers.find(t=>!t.cleared && !t.ran && t.ms===1500);assert.ok(t);t.ran=true;t.fn();await flush();}
  };
}
(async()=>{
  const h=harness(); await flush();
  assert.equal(h.$('localWaterDistance').textContent,'400.0');
  assert.equal(h.$('localWaterLevel').textContent,'\u2014'); assert.equal(h.$('useWaterReading').disabled,true);
  h.$('waterReferenceInput').value='450'; const calibration=h.fire('waterReferenceForm','submit');
  assert.equal(h.$('waterReferenceInput').disabled,true,'reference cannot be edited during POST');
  await calibration; assert.equal(h.$('waterReferenceInput').disabled,false);
  assert.equal(h.$('localWaterLevel').textContent,'50.0'); assert.equal(h.$('useWaterReading').disabled,false);
  h.fire('useWaterReading'); assert.equal(h.$('water').value,'50.0'); assert.equal(h.$('waterSource').value,'jsn');
  h.$('water').value='42'; h.$('fieldForm').listeners.get('input')({target:h.$('water')});
  assert.equal(h.$('waterSource').value,'manual');
  const pending=h.fire('getAdvisory'); await flush(); await h.nextAdvicePoll(); await pending;
  assert.match(h.$('advisoryStatus').textContent,/Review required/);
  assert.equal(h.$('advisoryExplanation').textContent,'<script>untrusted text</script>');
  assert.equal(h.$('advisoryLimitations').children.length,1);
  h.$('fieldForm').listeners.get('input')({target:h.$('crop')});
  assert.equal(h.$('advisoryExplanation').textContent,'');
  const old=h.fire('getAdvisory'); await flush();
  h.$('fieldForm').listeners.get('input')({target:h.$('plotId')}); await h.nextAdvicePoll(); await old;
  assert.equal(h.$('advisoryExplanation').textContent,'','late result cannot replace changed form');
  h.state.now=5200; h.intervals.find(t=>t.ms===500).fn();
  assert.equal(h.$('useWaterReading').disabled,true); assert.equal(h.$('localWaterLevel').textContent,'\u2014');
  h.state.water={...h.state.water,water_sequence:2,water_sensor_ok:false,water_distance_mm:null,water_level_valid:false,water_level_mm:null};
  await h.poll(); assert.equal(h.$('useWaterReading').disabled,true);
  h.state.water={...defaultWater(),water_sequence:3,water_reference_height_mm:400,water_level_mm:0,water_level_valid:true};
  await h.poll(); assert.equal(h.$('localWaterLevel').textContent,'0.0','genuine zero remains valid');
  h.state.water={...h.state.water,water_sequence:4,water_sample_age_ms:3400};
  await h.poll(); h.state.now+=101; h.intervals.find(t=>t.ms===500).fn();
  assert.equal(h.$('useWaterReading').disabled,true,'expiry includes age already elapsed on device');
  h.state.water={...h.state.water,water_sequence:5,water_sample_age_ms:0}; await h.poll();
  h.events.get('efb-weather-busy')({detail:1}); assert.equal(h.$('useWaterReading').disabled,true);
  assert.equal(h.$('getAdvisory').disabled,true);
  h.events.get('efb-weather-busy')({detail:0}); await flush();
  h.state.adviceError=true; const err=h.fire('getAdvisory'); await flush(); await h.nextAdvicePoll(); await err;
  assert.match(h.$('advisoryStatus').textContent,/rate limited/);
  console.log('PASS: field placement/area validation, CSV alignment, snapshot source guards, JSN calibration/manual selection/null/stale/zero, Groq async success/error/form-change race and safe text rendering.');
})().catch(error=>{console.error(error);process.exitCode=1;});

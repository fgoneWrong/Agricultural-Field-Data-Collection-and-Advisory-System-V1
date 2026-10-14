#pragma once
#include <Arduino.h>

// Served as a same-origin script, never contains credentials.
const char fieldExtensionsScript[] PROGMEM = R"JS(
(() => {
  'use strict';
  const el = id => document.getElementById(id);
  const form = el('fieldForm');
  let weatherBusy = false, waterInFlight = false, applyingReference = false;
  let waterData = null, waterReceivedAt = 0, lastSequence = null, lastUptime = 0, sequenceAt = 0;
  let referenceRevision = 0, referenceDirty = false;
  let fieldRevision = 0, advisoryRunning = false, advisoryGeneration = 0;
  const message = (id, text, kind = '') => {
    el(id).textContent = text;
    el(id).className = 'weather-status' + (kind ? ' ' + kind : '');
  };
  async function jsonRequest(url, options = {}) {
    const controller = new AbortController();
    const timer = window.setTimeout(() => controller.abort(), 6000);
    try {
      const response = await fetch(url, {...options, cache: 'no-store', signal: controller.signal});
      const data = await response.json();
      if (!response.ok || data.ok !== true) throw new Error(data.error || 'The ESP32 rejected the request.');
      return data;
    } finally { window.clearTimeout(timer); }
  }
  function clearWater(text) {
    waterData = null;
    el('localWaterDistance').textContent = '\u2014';
    el('localWaterLevel').textContent = '\u2014';
    el('useWaterReading').disabled = true;
    message('localWaterStatus', text, 'error');
  }
  function renderWater(data) {
    if (typeof data.water_sensor_ok !== 'boolean' || typeof data.water_level_valid !== 'boolean' ||
        typeof data.water_sensor_status !== 'string' || !Number.isInteger(data.water_sequence) ||
        !Number.isFinite(data.uptime_ms)) throw new Error('Invalid JSN sensor response.');
    const now = performance.now();
    if (lastSequence === null || data.water_sequence !== lastSequence || data.uptime_ms < lastUptime) sequenceAt = now;
    lastSequence = data.water_sequence; lastUptime = data.uptime_ms;
    const reference = data.water_reference_height_mm;
    el('waterReferenceValue').textContent = Number.isFinite(reference) ? reference.toFixed(1) + ' mm' : 'Not configured';
    if (!referenceDirty) el('waterReferenceInput').value = Number.isFinite(reference) ? reference.toFixed(1) : '';
    if (!data.water_sensor_ok || !Number.isFinite(data.water_sample_age_ms) || data.water_sample_age_ms < 0 ||
        data.water_sample_age_ms > 3500 || now - sequenceAt >= 5000 ||
        !Number.isFinite(data.water_distance_mm) || data.water_distance_mm < 230 || data.water_distance_mm > 5000) {
      clearWater(data.water_sensor_status || 'JSN reading unavailable or stale.'); return;
    }
    // Freshness includes time already elapsed on the device, not just transit.
    waterData = data; waterReceivedAt = now - data.water_sample_age_ms;
    el('localWaterDistance').textContent = data.water_distance_mm.toFixed(1);
    const levelValid = data.water_level_valid && Number.isFinite(reference) && reference >= 230 && reference <= 5000 &&
      Number.isFinite(data.water_level_mm) && data.water_level_mm >= 0 && data.water_level_mm <= reference &&
      Math.abs(reference - data.water_distance_mm - data.water_level_mm) < 0.2;
    el('localWaterLevel').textContent = levelValid ? data.water_level_mm.toFixed(1) : '\u2014';
    el('useWaterReading').disabled = !levelValid;
    message('localWaterStatus', levelValid ? 'Live JSN surface-water estimate. Check mounting and calibration before use.' :
      'Live distance available. Set a valid zero-water reference to obtain water depth.', levelValid ? '' : 'loading');
  }
  async function pollWater() {
    if (waterInFlight || applyingReference || weatherBusy || document.hidden) return;
    waterInFlight = true;
    const revision = referenceRevision;
    try {
      const data = await jsonRequest('/api/local-water');
      if (!weatherBusy && !applyingReference && revision === referenceRevision) renderWater(data);
    } catch (_) { if (revision === referenceRevision) clearWater('JSN unavailable: no fresh response. Check the device and wiring.'); }
    finally { waterInFlight = false; }
  }
  function invalidateAdvisory() {
    ++fieldRevision; ++advisoryGeneration;
    advisoryRunning = false; el('getAdvisory').disabled = weatherBusy;
    el('advisoryExplanation').textContent = '';
    el('advisoryLimitations').replaceChildren();
    el('advisoryMeta').textContent = '';
    message('advisoryStatus', 'Field information changed. Request a new review when ready.');
  }
  form.addEventListener('input', event => {
    if (event.target.form !== form) return;
    if (event.target.id === 'water') el('waterSource').value = 'manual';
    invalidateAdvisory();
  });
  form.addEventListener('change', event => { if (event.target.form === form) invalidateAdvisory(); });
  el('useWaterReading').addEventListener('click', () => {
    if (!waterData || el('useWaterReading').disabled || performance.now() - waterReceivedAt > 3500) {
      clearWater('Wait for a fresh, calibrated water-level reading.'); return;
    }
    el('water').value = waterData.water_level_mm.toFixed(1);
    el('waterSource').value = 'jsn';
    invalidateAdvisory();
    message('waterSourceNote', 'JSN selected. Saving or requesting advice uses a fresh server-side reading. Edit the number to return to manual entry.');
  });
  el('water').addEventListener('input', () => message('waterSourceNote', 'Manual field measurement selected.'));
  el('waterReferenceInput').addEventListener('input', () => { referenceDirty = true; });
  el('waterReferenceForm').addEventListener('submit', async event => {
    event.preventDefault();
    if (applyingReference || weatherBusy || !el('waterReferenceInput').reportValidity()) return;
    applyingReference = true; ++referenceRevision;
    el('waterReferenceInput').disabled = true;
    el('applyWaterReference').disabled = true;
    if (el('waterSource').value === 'jsn') { el('water').value = ''; el('waterSource').value = 'manual'; }
    invalidateAdvisory(); clearWater('Applying zero-water reference...');
    try {
      const data = await jsonRequest('/api/local-water/reference', {
        method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded', 'X-EFB-Request': '1'},
        body: new URLSearchParams({reference_height_mm: el('waterReferenceInput').value}).toString()
      });
      referenceDirty = false; renderWater(data);
      message('waterReferenceMessage', 'Reference applied in RAM. Re-enter after restarting or moving the probe.');
    } catch (error) { message('waterReferenceMessage', error.message, 'error'); }
    finally {
      applyingReference = false;
      el('waterReferenceInput').disabled = weatherBusy;
      el('applyWaterReference').disabled = weatherBusy;
    }
  });
  function renderAdvisory(data) {
    // Fixed status/quantity are never controlled by model-generated text.
    if (data.status !== 'review_required' || data.irrigation_minutes !== null) throw new Error('Invalid advisory safety state.');
    message('advisoryStatus', 'Review required \u2014 irrigation action and minutes are not yet determined.', 'loading');
    el('advisoryExplanation').textContent = data.explanation || '';
    el('advisoryLimitations').replaceChildren();
    if (Array.isArray(data.limitations)) data.limitations.slice(0, 10).forEach(text => {
      if (typeof text !== 'string') return;
      const item = document.createElement('li'); item.textContent = text;
      el('advisoryLimitations').appendChild(item);
    });
    el('advisoryMeta').textContent = 'Snapshot review received ' + new Date().toLocaleString('en-GB', {hour12: false}) +
      '. AI text is unverified and is not a pump command. Recheck current field conditions.';
  }
  el('getAdvisory').addEventListener('click', async () => {
    if (advisoryRunning || weatherBusy || !form.reportValidity()) return;
    const generation = ++advisoryGeneration, revision = fieldRevision;
    advisoryRunning = true; el('getAdvisory').disabled = true;
    el('advisoryExplanation').textContent = ''; el('advisoryLimitations').replaceChildren(); el('advisoryMeta').textContent = '';
    message('advisoryStatus', 'Sending a field snapshot for Groq review...', 'loading');
    try {
      const body = new URLSearchParams(new FormData(form));
      const accepted = await jsonRequest('/api/advisory', {
        method: 'POST', headers: {'Content-Type': 'application/x-www-form-urlencoded', 'X-EFB-Request': '1'}, body: body.toString()
      });
      const requestId = accepted.request_id;
      if (!Number.isInteger(requestId) || requestId <= 0) throw new Error('Missing review request identifier.');
      const startedAt = performance.now();
      while (generation === advisoryGeneration && revision === fieldRevision) {
        if (performance.now() - startedAt > 90000) throw new Error('Review timed out. The local measurements remain available.');
        await new Promise(resolve => window.setTimeout(resolve, 1500));
        if (generation !== advisoryGeneration || revision !== fieldRevision) return;
        if (weatherBusy) continue;
        const data = await jsonRequest('/api/advisory');
        if (generation !== advisoryGeneration || revision !== fieldRevision) return;
        if (data.request_id !== requestId) throw new Error('Another browser replaced this review. Request a new review for your plot.');
        if (data.state === 'complete') {
          renderAdvisory(data);
          window.setTimeout(() => { if (generation === advisoryGeneration) invalidateAdvisory(); }, 300000);
          return;
        }
        if (data.state === 'error') throw new Error(data.error || 'Groq is temporarily unavailable. No irrigation decision was made.');
      }
    } catch (error) {
      if (generation === advisoryGeneration) message('advisoryStatus', error.message, 'error');
    } finally {
      if (generation === advisoryGeneration) { advisoryRunning = false; el('getAdvisory').disabled = weatherBusy; }
    }
  });
  window.addEventListener('efb-weather-busy', event => {
    weatherBusy = Number(event.detail) > 0;
    el('waterReferenceInput').disabled = weatherBusy || applyingReference;
    el('applyWaterReference').disabled = weatherBusy || applyingReference;
    el('getAdvisory').disabled = weatherBusy || advisoryRunning;
    if (weatherBusy) clearWater('Display paused while weather loads. JSN sampling continues.');
    else pollWater();
  });
  window.setInterval(() => {
    if (waterData && performance.now() - waterReceivedAt > 3500) clearWater('JSN display is stale. Waiting for a fresh sample.');
  }, 500);
  window.setInterval(pollWater, 1500);
  pollWater();
})();
)JS";

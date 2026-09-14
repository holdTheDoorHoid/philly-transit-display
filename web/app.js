// Philly Transit Display — web UI.
// Vanilla JS, ES2020, no framework, no build step. Talks to the device HTTP API
// described in DESIGN.md §7. Hash-routed single page app.
'use strict';

/* ======================== tiny DOM helpers ======================== */

function h(tag, attrs, ...children) {
  const el = document.createElement(tag);
  applyAttrs(el, attrs);
  appendChildren(el, children);
  return el;
}
function hs(tag, attrs, ...children) {
  const el = document.createElementNS('http://www.w3.org/2000/svg', tag);
  applyAttrs(el, attrs);
  appendChildren(el, children);
  return el;
}
function applyAttrs(el, attrs) {
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') el.setAttribute('class', v);
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else if (v === true) el.setAttribute(k, '');
    else el.setAttribute(k, String(v));
  }
}
function appendChildren(el, children) {
  for (const c of children.flat(Infinity)) {
    if (c == null || c === false) continue;
    el.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
}
function clear(el) { while (el.firstChild) el.removeChild(el.firstChild); }
function $(sel, root) { return (root || document).querySelector(sel); }

/* ======================== formatting helpers ======================== */

function fmtClock(epochSec) {
  if (!epochSec) return '--:--';
  const d = new Date(epochSec * 1000);
  let hh = d.getHours();
  const mm = String(d.getMinutes()).padStart(2, '0');
  const ap = hh >= 12 ? 'PM' : 'AM';
  hh = hh % 12; if (hh === 0) hh = 12;
  return `${hh}:${mm} ${ap}`;
}
function fmtAgo(sec) {
  if (sec == null) return '--';
  if (sec < 60) return `${Math.round(sec)}s ago`;
  if (sec < 3600) return `${Math.round(sec / 60)}m ago`;
  return `${Math.round(sec / 3600)}h ago`;
}
function fmtEtaMinutes(etaS) {
  if (etaS == null) return '--';
  if (etaS <= 0) return 'Now';
  if (etaS < 60) return 'Due';
  return String(Math.round(etaS / 60));
}
function humanBytes(n) {
  if (n == null) return '--';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(1)} MB`;
}
function slugify(s) {
  return String(s).toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '') || 'stop';
}
function debounce(fn, ms) {
  let t;
  return (...args) => { clearTimeout(t); t = setTimeout(() => fn(...args), ms); };
}

/* ======================== API layer (DESIGN.md §7) ======================== */

class ApiError extends Error {
  constructor(message, path, status) { super(message); this.path = path; this.status = status; }
}

async function fetchJSON(url, opts) {
  let res;
  try {
    res = await fetch(url, opts);
  } catch (e) {
    throw new ApiError('Network error: could not reach the device.', null, 0);
  }
  let body = null;
  const ct = res.headers.get('content-type') || '';
  if (ct.includes('application/json')) {
    try { body = await res.json(); } catch (e) { /* ignore parse failure */ }
  }
  if (!res.ok) {
    const msg = (body && body.error) || `Request failed (HTTP ${res.status})`;
    throw new ApiError(msg, body && body.path, res.status);
  }
  return body;
}

const api = {
  state: () => fetchJSON('/api/state'),
  config: () => fetchJSON('/api/config'),
  saveConfig: (cfg) => fetchJSON('/api/config', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cfg),
  }),
  proxyStops: (route) => fetchJSON(`/api/proxy/stops?route=${encodeURIComponent(route)}`),
  proxySchedule: (stopId) => fetchJSON(`/api/proxy/schedule?stop_id=${encodeURIComponent(stopId)}`),
  railStations: () => fetchJSON('/api/rail/stations'),
  stats: (stop, days) => fetchJSON(`/api/stats?stop=${encodeURIComponent(stop)}&days=${encodeURIComponent(days)}`),
  logIndex: () => fetchJSON('/api/log/index'),
  reboot: () => fetchJSON('/api/reboot', { method: 'POST' }),
  wifiReset: () => fetchJSON('/api/wifi/reset', { method: 'POST' }),
};

function uploadFirmware(file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) onProgress(e.loaded / e.total); };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) resolve();
      else {
        let msg = `OTA failed (HTTP ${xhr.status})`;
        try { const b = JSON.parse(xhr.responseText); if (b && b.error) msg = b.error; } catch (e) {}
        reject(new Error(msg));
      }
    };
    xhr.onerror = () => reject(new Error('Network error during upload.'));
    const fd = new FormData();
    fd.append('firmware', file, file.name);
    xhr.send(fd);
  });
}

/* ======================== Leaflet loader (CDN, graceful degrade) ======================== */

let leafletPromise = null;
function loadLeaflet() {
  if (leafletPromise) return leafletPromise;
  leafletPromise = new Promise((resolve) => {
    if (window.L) { resolve(window.L); return; }
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';
    document.head.appendChild(link);
    const script = document.createElement('script');
    script.src = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
    let done = false;
    const finish = (ok) => { if (done) return; done = true; resolve(ok ? window.L : null); };
    script.onload = () => finish(!!window.L);
    script.onerror = () => finish(false);
    document.head.appendChild(script);
    setTimeout(() => finish(!!window.L), 6000);
  });
  return leafletPromise;
}

/* ======================== router ======================== */

const routes = { now: renderNow, stops: renderStops, stats: renderStats, settings: renderSettings };
let activeCleanup = null;

function router() {
  if (!location.hash) { location.hash = '#/now'; return; }
  if (typeof activeCleanup === 'function') { try { activeCleanup(); } catch (e) {} }
  activeCleanup = null;
  const name = (location.hash.replace(/^#\//, '').split('/')[0]) || 'now';
  const view = routes[name] || renderNow;
  for (const a of document.querySelectorAll('#mainnav a')) {
    a.classList.toggle('active', a.dataset.route === name);
  }
  const root = $('#view');
  clear(root);
  view(root);
}
window.addEventListener('hashchange', router);
window.addEventListener('DOMContentLoaded', router);

/* ======================== Now view ======================== */

function renderNow(root) {
  const wrap = h('div', { class: 'stack' });
  const banner = h('div', { id: 'now-banner' });
  const strip = h('div', { id: 'status-strip', class: 'card' }, 'Loading device status…');
  const stops = h('div', { id: 'now-stops', class: 'stack' });
  const alerts = h('div', { id: 'now-alerts', class: 'card hidden' },
    h('h2', {}, 'Service alerts'), h('div', { id: 'alerts-list' }));
  wrap.append(banner, strip, stops, alerts);
  root.append(wrap);

  let stopped = false;
  async function tick() {
    if (stopped) return;
    try {
      const state = await api.state();
      if (stopped) return;
      renderStatusStrip(strip, state);
      renderStaleBanner(banner, state);
      renderStopPanels(stops, state.stops || []);
      renderAlerts(alerts, state.alerts || []);
    } catch (e) {
      clear(banner);
      banner.append(h('div', { class: 'banner danger', role: 'alert' }, e.message || 'Could not load state.'));
    }
  }
  tick();
  const timer = setInterval(tick, 15000);
  activeCleanup = () => { stopped = true; clearInterval(timer); };
}

function renderStaleBanner(banner, state) {
  clear(banner);
  const lp = state.last_poll || {};
  const stale = lp.ok === false || (lp.age_s != null && lp.age_s > 90);
  if (stale) {
    const mins = lp.age_s != null ? Math.round(lp.age_s / 60) : null;
    const text = lp.ok === false
      ? `Stale data — last poll failed${lp.error ? `: ${lp.error}` : ''}`
      : `Stale data — updated ${mins} min ago`;
    banner.append(h('div', { class: 'banner warn', role: 'alert' }, text));
  }
}

function renderStatusStrip(strip, state) {
  clear(strip);
  const wifi = state.wifi || {};
  const sd = state.sd || {};
  const lp = state.last_poll || {};
  const tiles = [
    ['Wi-Fi', wifi.ssid ? `${wifi.ssid} (${wifi.rssi ?? '?'} dBm)` : 'Disconnected'],
    ['IP', wifi.ip || '--'],
    ['SD card', sd.mounted ? `${humanBytes((sd.free_mb || 0) * 1024 * 1024)} free` : 'Not mounted'],
    ['Heap free', humanBytes(state.heap)],
    ['Last poll', lp.ok === false ? 'Failed' : fmtAgo(lp.age_s)],
    ['Uptime', fmtAgo(state.uptime)],
  ];
  for (const [k, v] of tiles) {
    strip.append(h('div', { class: 'status-tile' }, h('div', { class: 'k' }, k), h('div', { class: 'v' }, v)));
  }
}

function badgeFor(a) {
  if (a.status === 'skipped') return { cls: 'skip', text: 'skip', aria: 'Skipped — detour' };
  if (a.status !== 'live' || !a.late_known) return { cls: 'sched', text: 'sched', aria: 'Scheduled, no live tracking' };
  const late = a.late_min || 0;
  if (late > 5) return { cls: 'late', text: `+${late}`, aria: `${late} minutes late` };
  if (late < -1) return { cls: 'early', text: `−${Math.abs(late)}`, aria: `${Math.abs(late)} minutes early` };
  return { cls: 'on-time', text: 'on time', aria: 'On time' };
}

function renderStopPanels(container, stopSnaps) {
  clear(container);
  if (!stopSnaps.length) {
    container.append(h('div', { class: 'card muted' }, 'No stops configured yet. Add one from the Stops tab.'));
    return;
  }
  for (const s of stopSnaps) {
    const title = `${s.route || (s.mode === 'rail' ? 'Rail' : '')}${s.headsign ? ' → ' + s.headsign : ''}${s.stop_name ? ' · ' + s.stop_name : ''}`.trim()
      || s.label || s.key;
    const panel = h('div', { class: 'card stop-panel' }, h('div', { class: 'title' }, s.label ? `${s.label} — ${title}` : title));
    if (!s.ok) {
      panel.append(h('div', { class: 'banner danger' }, s.error || 'No data for this stop.'));
    } else if (!s.arrivals || !s.arrivals.length) {
      panel.append(h('div', { class: 'muted small' }, 'No upcoming arrivals.'));
    } else {
      for (const a of s.arrivals.slice(0, s.show || 4)) {
        const badge = badgeFor(a);
        panel.append(h('div', { class: 'arrival-row' },
          h('span', { class: 'route-badge' }, s.route || (s.mode === 'rail' ? 'RR' : '?')),
          h('span', { class: 'arrival-dest' }, a.destination || s.headsign || ''),
          h('span', { class: 'arrival-minutes' }, fmtEtaMinutes(a.eta_s)),
          h('span', { class: `status-badge ${badge.cls}`, title: badge.aria, 'aria-label': badge.aria }, badge.text),
        ));
      }
    }
    container.append(panel);
  }
}

function renderAlerts(box, alerts) {
  const list = $('#alerts-list', box);
  clear(list);
  if (!alerts.length) { box.classList.add('hidden'); return; }
  box.classList.remove('hidden');
  for (const a of alerts) {
    const item = h('div', { class: 'alert-item' },
      h('span', { class: 'alert-route' }, a.route), h('span', {}, a.text));
    if (a.detours && a.detours.length) {
      const ul = h('ul', { class: 'small muted' });
      for (const d of a.detours) ul.append(h('li', {}, d));
      item.append(ul);
    }
    list.append(item);
  }
}

/* ======================== Stops view ======================== */

const US_TIMEZONES = [
  ['Eastern (New York)', 'EST5EDT,M3.2.0,M11.1.0'],
  ['Central (Chicago)', 'CST6CDT,M3.2.0,M11.1.0'],
  ['Mountain (Denver)', 'MST7MDT,M3.2.0,M11.1.0'],
  ['Arizona (no DST)', 'MST7'],
  ['Pacific (Los Angeles)', 'PST8PDT,M3.2.0,M11.1.0'],
  ['Alaska', 'AKST9AKDT,M3.2.0,M11.1.0'],
  ['Hawaii (no DST)', 'HST10'],
];

let liveConfig = null;
let wizard = null;
let stopsRoot = null;
let currentMap = null;
let mapMarkers = [];
// True only while the Stops view is the mounted view. Guards async continuations
// (config fetches, wizard lookups, saves) from touching #view after the user has
// already navigated to a different tab and a new view has been rendered there.
let stopsActive = false;

function resetMap() {
  if (currentMap) { try { currentMap.remove(); } catch (e) {} currentMap = null; }
  mapMarkers = [];
}

async function renderStops(root) {
  stopsRoot = root;
  wizard = null;
  editingIndex = -1;
  stopsActive = true;
  activeCleanup = () => { stopsActive = false; resetMap(); };
  root.append(h('p', { class: 'muted' }, 'Loading configuration…'));
  let cfg;
  try {
    cfg = await api.config();
  } catch (e) {
    if (!stopsActive) return;
    clear(root);
    root.append(h('div', { class: 'banner danger' }, e.message));
    return;
  }
  if (!stopsActive) return;
  liveConfig = cfg;
  drawStops();
}

function drawStops() {
  clear(stopsRoot);
  const wrap = h('div', { class: 'stack' });
  wrap.append(h('div', { id: 'stops-banner' }));
  if (wizard) {
    wrap.append(drawWizard());
  } else {
    wrap.append(drawStopList());
  }
  stopsRoot.append(wrap);
}

function stopsBanner(msg, cls) {
  const b = $('#stops-banner', stopsRoot);
  if (!b) return;
  clear(b);
  if (msg) b.append(h('div', { class: `banner ${cls || 'danger'}`, role: 'alert' }, msg));
}

function stopTitle(s) {
  if (s.mode === 'rail') {
    const dir = s.direction === 'N' ? 'Northbound' : s.direction === 'S' ? 'Southbound' : 'Both directions';
    return `${s.label || 'Regional Rail'} — ${s.station} (${dir})`;
  }
  return `${s.label || s.route} — ${s.route} → ${s.headsign || '?'} · ${s.stop_name || s.stop_id}`;
}

function drawStopList() {
  const card = h('div', { class: 'card' });
  card.append(h('div', { class: 'row between' },
    h('h2', {}, `Configured stops (${liveConfig.stops.length}/8)`),
    h('button', {
      class: 'primary', disabled: liveConfig.stops.length >= 8,
      onclick: () => { wizard = { step: 'mode' }; drawStops(); },
    }, '+ Add stop')));
  if (liveConfig.stops.length >= 8) {
    card.append(h('p', { class: 'small muted' }, 'Maximum of 8 stops reached. Remove one to add another.'));
  }
  const list = h('div', {});
  liveConfig.stops.forEach((s, i) => {
    if (editingIndex === i) { list.append(drawEditForm(s, i)); return; }
    list.append(h('div', { class: 'stop-list-item' },
      h('div', { class: 'meta' },
        h('div', { class: 'name' }, stopTitle(s)),
        h('div', { class: 'small muted' }, `key: ${s.key} · shows ${s.show} row${s.show === 1 ? '' : 's'}`)),
      h('div', { class: 'actions' },
        h('button', { class: 'icon', title: 'Move up', 'aria-label': `Move ${s.label} up`, disabled: i === 0, onclick: () => moveStop(i, -1) }, '↑'),
        h('button', { class: 'icon', title: 'Move down', 'aria-label': `Move ${s.label} down`, disabled: i === liveConfig.stops.length - 1, onclick: () => moveStop(i, 1) }, '↓'),
        h('button', { class: 'icon', title: 'Edit', 'aria-label': `Edit ${s.label}`, onclick: () => { editingIndex = i; drawStops(); } }, '✎'),
        h('button', { class: 'icon danger', title: 'Remove', 'aria-label': `Remove ${s.label}`, onclick: () => removeStop(i) }, '✕'),
      )));
  });
  if (!liveConfig.stops.length) list.append(h('p', { class: 'muted' }, 'No stops yet. Add one to get started.'));
  card.append(list);
  return card;
}

let editingIndex = -1;

function drawEditForm(s, i) {
  const labelInput = h('input', { type: 'text', value: s.label, id: `edit-label-${i}` });
  const showInput = h('select', { id: `edit-show-${i}` },
    ...[1, 2, 3, 4].map((n) => h('option', { value: n, selected: n === s.show || undefined }, `${n} row${n === 1 ? '' : 's'}`)));
  const fields = [
    h('label', { for: `edit-label-${i}` }, 'Label'), labelInput,
    h('label', { for: `edit-show-${i}` }, 'Rows to show'), showInput,
  ];
  if (s.mode === 'rail') {
    const dirSel = h('select', { id: `edit-dir-${i}` },
      h('option', { value: 'N', selected: s.direction === 'N' || undefined }, 'Northbound'),
      h('option', { value: 'S', selected: s.direction === 'S' || undefined }, 'Southbound'),
      h('option', { value: '', selected: !s.direction || undefined }, 'Both'));
    const lineInput = h('input', { type: 'text', id: `edit-line-${i}`, value: s.line || '', placeholder: 'e.g. Paoli/Thorndale (optional)' });
    fields.push(h('label', { for: `edit-dir-${i}` }, 'Direction'), dirSel);
    fields.push(h('label', { for: `edit-line-${i}` }, 'Line filter'), lineInput);
    fields._dirSel = dirSel; fields._lineInput = lineInput;
  }
  const form = h('div', { class: 'card' }, h('h3', {}, `Edit: ${s.label}`), ...fields,
    h('div', { class: 'row', style: 'margin-top:.6rem' },
      h('button', { class: 'primary', onclick: async () => {
        const updated = { ...s, label: labelInput.value.trim() || s.label, show: Number(showInput.value) };
        if (s.mode === 'rail') { updated.direction = fields._dirSel.value; updated.line = fields._lineInput.value.trim(); }
        const next = liveConfig.stops.slice();
        next[i] = updated;
        await persistConfig({ ...liveConfig, stops: next }, () => { editingIndex = -1; });
      } }, 'Save'),
      h('button', { onclick: () => { editingIndex = -1; drawStops(); } }, 'Cancel')));
  return form;
}

async function persistConfig(cfg, onOk) {
  try {
    const saved = await api.saveConfig(cfg);
    if (!stopsActive) return;
    liveConfig = saved || cfg;
    if (onOk) onOk();
    drawStops();
    stopsBanner('Saved.', 'ok');
  } catch (e) {
    if (!stopsActive) return;
    drawStops();
    stopsBanner(e.path ? `${e.message} (${e.path})` : e.message, 'danger');
  }
}

function moveStop(i, delta) {
  const j = i + delta;
  if (j < 0 || j >= liveConfig.stops.length) return;
  const next = liveConfig.stops.slice();
  [next[i], next[j]] = [next[j], next[i]];
  persistConfig({ ...liveConfig, stops: next });
}

function removeStop(i) {
  const s = liveConfig.stops[i];
  if (!confirm(`Remove "${s.label}" from your display?`)) return;
  const next = liveConfig.stops.slice();
  next.splice(i, 1);
  persistConfig({ ...liveConfig, stops: next });
}

/* ---- Add-stop wizard ---- */

function wizardStepsBar(current, stepsList) {
  return h('div', { class: 'wizard-steps' }, ...stepsList.map((s) =>
    h('span', { class: `step${s === current ? ' active' : ''}` }, s)));
}

function drawWizard() {
  const card = h('div', { class: 'card' });
  card.append(h('div', { class: 'row between' }, h('h2', {}, 'Add a stop'),
    h('button', { onclick: () => { wizard = null; resetMap(); drawStops(); } }, 'Cancel')));

  if (wizard.step === 'mode') {
    card.append(wizardStepsBar('mode', ['mode', 'find stop', 'direction', 'details']));
    card.append(h('div', { class: 'mode-choices' },
      modeButton('bus', 'Bus / Trolley', 'Enter a route number or letter (e.g. 17, T4, G1).'),
      modeButton('subway', 'Subway', 'Broad Street or Market-Frankford Line — schedule only.'),
      modeButton('rail', 'Regional Rail', 'Pick a station from SEPTA’s Regional Rail list.'),
    ));
    return card;
  }

  if (wizard.step === 'route') {
    card.append(wizardStepsBar('find stop', ['mode', 'find stop', 'direction', 'details']));
    const input = h('input', { type: 'text', id: 'route-input', placeholder: 'e.g. 17', value: wizard.route || '' });
    card.append(h('label', { for: 'route-input' }, `${wizard.mode === 'subway' ? 'Subway ' : ''}Route ID`));
    card.append(input);
    card.append(h('p', { class: 'small muted' }, 'SEPTA route IDs are strings, not numbers (e.g. "T4", "G1").'));
    const err = h('div', {});
    card.append(err);
    card.append(h('div', { class: 'row', style: 'margin-top:.6rem' }, h('button', {
      class: 'primary',
      onclick: async () => {
        const route = input.value.trim();
        if (!route) { clear(err); err.append(h('div', { class: 'field-error' }, 'Enter a route id.')); return; }
        clear(err); err.append(h('p', { class: 'muted small' }, 'Looking up stops…'));
        try {
          const stops = await api.proxyStops(route);
          if (!stopsActive) return;
          wizard.route = route;
          wizard.stopResults = stops || [];
          if (!wizard.stopResults.length) {
            if (wizard.mode === 'subway') { wizard.step = 'manual-stop'; }
            else { clear(err); err.append(h('div', { class: 'field-error' }, `No stops found for route "${route}". Check the route id and try again.`)); return; }
          } else {
            wizard.step = 'pick-stop';
          }
          drawStops();
        } catch (e) {
          if (!stopsActive) return;
          clear(err); err.append(h('div', { class: 'field-error' }, e.message));
        }
      },
    }, 'Find stops')));
    return card;
  }

  if (wizard.step === 'manual-stop') {
    card.append(wizardStepsBar('find stop', ['mode', 'find stop', 'direction', 'details']));
    card.append(h('div', { class: 'banner warn' },
      'SEPTA doesn’t list individual subway stops through the Stops API. If you know the numeric stop_id (from GTFS static data), enter it below. Otherwise this stop isn’t supported yet.'));
    const idInput = h('input', { type: 'text', id: 'manual-stopid', placeholder: 'e.g. 90005' });
    const nameInput = h('input', { type: 'text', id: 'manual-stopname', placeholder: 'e.g. City Hall' });
    card.append(h('label', { for: 'manual-stopid' }, 'Stop ID'), idInput);
    card.append(h('label', { for: 'manual-stopname' }, 'Stop name'), nameInput);
    const err = h('div', {});
    card.append(err);
    card.append(h('div', { class: 'row', style: 'margin-top:.6rem' },
      h('button', { onclick: () => { wizard.step = 'route'; drawStops(); } }, 'Back'),
      h('button', { class: 'primary', onclick: () => {
        if (!idInput.value.trim()) { clear(err); err.append(h('div', { class: 'field-error' }, 'Enter a stop id.')); return; }
        wizard.stopId = idInput.value.trim();
        wizard.stopName = nameInput.value.trim() || `Stop ${wizard.stopId}`;
        wizard.step = 'schedule';
        drawStops();
      } }, 'Next')));
    return card;
  }

  if (wizard.step === 'pick-stop') {
    card.append(wizardStepsBar('find stop', ['mode', 'find stop', 'direction', 'details']));
    const search = h('input', { type: 'search', placeholder: 'Filter by cross streets…', 'aria-label': 'Filter stops' });
    card.append(search);
    const mapDiv = h('div', { id: 'stop-map' });
    card.append(mapDiv);
    const results = h('div', { id: 'stop-search-results' });
    card.append(results);
    const nextBtn = h('button', { class: 'primary', disabled: !wizard.stopId, style: 'margin-top:.6rem' }, 'Next: directions');
    nextBtn.addEventListener('click', () => { wizard.step = 'schedule'; drawStops(); });
    card.append(nextBtn);

    const items = new Map();
    function selectStop(entry) {
      wizard.stopId = entry.stopid;
      wizard.stopName = entry.stopname;
      wizard.lat = entry.lat; wizard.lng = entry.lng;
      nextBtn.disabled = false;
      for (const [id, btn] of items) btn.classList.toggle('selected', id === entry.stopid);
      for (const m of mapMarkers) m.marker.setStyle ? null : null;
      const found = mapMarkers.find((m) => m.entry.stopid === entry.stopid);
      if (found && found.marker.openTooltip) found.marker.openTooltip();
    }

    for (const entry of wizard.stopResults) {
      const btn = h('button', { class: `result-item${wizard.stopId === entry.stopid ? ' selected' : ''}`, onclick: () => selectStop(entry) }, entry.stopname);
      items.set(entry.stopid, btn);
      results.append(btn);
    }

    search.addEventListener('input', debounce(() => {
      const q = search.value.trim().toLowerCase();
      for (const entry of wizard.stopResults) {
        const match = !q || entry.stopname.toLowerCase().includes(q);
        items.get(entry.stopid).classList.toggle('hidden', !match);
      }
      for (const m of mapMarkers) {
        const match = !q || m.entry.stopname.toLowerCase().includes(q);
        if (match) { if (!currentMap.hasLayer(m.marker)) m.marker.addTo(currentMap); }
        else if (currentMap.hasLayer(m.marker)) currentMap.removeLayer(m.marker);
      }
    }, 150));

    resetMap();
    loadLeaflet().then((L) => {
      if (!L || !wizard.stopResults.length) { mapDiv.classList.add('hidden'); return; }
      const pts = wizard.stopResults.map((e) => [+e.lat, +e.lng]).filter((p) => !isNaN(p[0]) && !isNaN(p[1]));
      if (!pts.length) { mapDiv.classList.add('hidden'); return; }
      const avgLat = pts.reduce((a, p) => a + p[0], 0) / pts.length;
      const avgLng = pts.reduce((a, p) => a + p[1], 0) / pts.length;
      currentMap = L.map(mapDiv).setView([avgLat, avgLng], 13);
      L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
        attribution: '&copy; OpenStreetMap contributors', maxZoom: 19,
      }).addTo(currentMap);
      for (const entry of wizard.stopResults) {
        const lat = +entry.lat, lng = +entry.lng;
        if (isNaN(lat) || isNaN(lng)) continue;
        const marker = L.marker([lat, lng]).addTo(currentMap).bindTooltip(entry.stopname);
        marker.on('click', () => selectStop(entry));
        mapMarkers.push({ entry, marker });
      }
    });
    return card;
  }

  if (wizard.step === 'schedule') {
    card.append(wizardStepsBar('direction', ['mode', 'find stop', 'direction', 'details']));
    card.append(h('p', {}, `Stop: ${wizard.stopName}`));
    const box = h('div', {}, h('p', { class: 'muted' }, 'Looking up directions served here…'));
    card.append(box);
    api.proxySchedule(wizard.stopId).then((sched) => {
      if (!stopsActive) return;
      clear(box);
      const routeKey = wizard.route ? Object.keys(sched || {}).find((k) => k === wizard.route) || Object.keys(sched || {})[0] : Object.keys(sched || {})[0];
      const entries = (sched && routeKey && sched[routeKey]) || [];
      if (!entries.length) {
        box.append(h('div', { class: 'banner warn' }, 'This stop isn’t supported yet — SEPTA has no schedule data for it.'));
        box.append(h('div', { class: 'row', style: 'margin-top:.6rem' },
          h('button', { onclick: () => { wizard.step = wizard.mode === 'subway' && !wizard.stopResults?.length ? 'manual-stop' : 'pick-stop'; drawStops(); } }, 'Try a different stop')));
        return;
      }
      if (!wizard.route) wizard.route = routeKey;
      const byDir = new Map();
      for (const e of entries) if (!byDir.has(e.Direction)) byDir.set(e.Direction, e.DirectionDesc);
      if (entries[0] && entries[0].StopName && !wizard.manualName) wizard.stopName = entries[0].StopName;
      const choices = h('div', { class: 'direction-choices' });
      for (const [dir, desc] of byDir) {
        const id = `dir-${dir}`;
        choices.append(h('label', { for: id },
          h('input', { type: 'radio', name: 'direction', id, value: dir, checked: wizard.direction === dir || undefined }),
          `${desc} (direction ${dir})`));
      }
      box.append(h('label', {}, 'Direction served at this stop'));
      box.append(choices);
      box.append(h('div', { class: 'row', style: 'margin-top:.6rem' },
        h('button', { onclick: () => { wizard.step = 'pick-stop'; drawStops(); } }, 'Back'),
        h('button', { class: 'primary', onclick: () => {
          const picked = choices.querySelector('input[name="direction"]:checked');
          if (!picked) return;
          wizard.direction = picked.value;
          wizard.directionDesc = byDir.get(picked.value);
          wizard.step = 'details';
          drawStops();
        } }, 'Next')));
    }).catch((e) => {
      if (!stopsActive) return;
      clear(box);
      box.append(h('div', { class: 'banner danger' }, e.message));
    });
    return card;
  }

  if (wizard.step === 'details') {
    card.append(wizardStepsBar('details', ['mode', 'find stop', 'direction', 'details']));
    const key = `${wizard.route}-${wizard.stopId}-${wizard.direction}`;
    const dup = liveConfig.stops.some((s) => s.key === key);
    const labelInput = h('input', { type: 'text', id: 'detail-label', value: `${wizard.route} → ${wizard.directionDesc}` });
    const showInput = h('select', { id: 'detail-show' }, ...[1, 2, 3, 4].map((n) => h('option', { value: n, selected: n === 3 || undefined }, `${n} row${n === 1 ? '' : 's'}`)));
    const nameInput = h('input', { type: 'text', id: 'detail-stopname', value: wizard.stopName });
    card.append(h('label', { for: 'detail-label' }, 'Label'), labelInput);
    card.append(h('label', { for: 'detail-stopname' }, 'Stop name (shown on display)'), nameInput);
    card.append(h('label', { for: 'detail-show' }, 'Rows to show'), showInput);
    if (dup) card.append(h('div', { class: 'banner warn' }, 'This exact stop and direction is already configured.'));
    const err = h('div', {});
    card.append(err);
    card.append(h('div', { class: 'row', style: 'margin-top:.6rem' },
      h('button', { onclick: () => { wizard.step = 'schedule'; drawStops(); } }, 'Back'),
      h('button', {
        class: 'primary', disabled: dup,
        onclick: async () => {
          const stop = {
            key, mode: wizard.mode, route: wizard.route, stop_id: String(wizard.stopId),
            direction: wizard.direction, headsign: wizard.directionDesc || '',
            label: labelInput.value.trim() || key, stop_name: nameInput.value.trim(),
            show: Number(showInput.value),
          };
          const next = { ...liveConfig, stops: [...liveConfig.stops, stop] };
          try {
            const saved = await api.saveConfig(next);
            if (!stopsActive) return;
            liveConfig = saved || next;
            wizard = null;
            resetMap();
            drawStops();
            stopsBanner('Stop added.', 'ok');
          } catch (e) {
            if (!stopsActive) return;
            clear(err); err.append(h('div', { class: 'field-error' }, e.path ? `${e.message} (${e.path})` : e.message));
          }
        },
      }, 'Save stop')));
    return card;
  }

  if (wizard.step === 'station') {
    card.append(wizardStepsBar('find stop', ['mode', 'station', 'direction', 'details']));
    const search = h('input', { type: 'search', id: 'rail-station-search', placeholder: 'Filter stations…' });
    card.append(h('label', { for: 'rail-station-search' }, 'Regional Rail station'));
    card.append(search);
    const results = h('div', { id: 'stop-search-results' });
    card.append(results);
    card.append(h('p', { class: 'small muted', id: 'station-hint' }, wizard.station ? `Selected: ${wizard.station}` : 'Pick a station from the list.'));
    const nextBtn = h('button', { class: 'primary', disabled: !wizard.station, style: 'margin-top:.6rem' },
      'Next: direction');
    nextBtn.addEventListener('click', () => { wizard.step = 'rail-direction'; drawStops(); });
    card.append(nextBtn);

    (wizard.stations ? Promise.resolve(wizard.stations) : api.railStations()).then((stations) => {
      if (!stopsActive) return;
      wizard.stations = stations;
      const names = stations.map((s) => (typeof s === 'string' ? s : s.name));
      function draw(filter) {
        clear(results);
        for (const name of names) {
          if (filter && !name.toLowerCase().includes(filter)) continue;
          results.append(h('button', { class: `result-item${wizard.station === name ? ' selected' : ''}`, onclick: () => {
            wizard.station = name;
            $('#station-hint', card).textContent = `Selected: ${name}`;
            nextBtn.disabled = false;
            for (const b of results.children) b.classList.toggle('selected', b.textContent === name);
          } }, name));
        }
      }
      draw('');
      search.addEventListener('input', debounce(() => draw(search.value.trim().toLowerCase()), 100));
    });
    return card;
  }

  if (wizard.step === 'rail-direction') {
    card.append(wizardStepsBar('direction', ['mode', 'station', 'direction', 'details']));
    const choices = h('div', { class: 'direction-choices' },
      h('label', {}, h('input', { type: 'radio', name: 'raildir', value: 'N', checked: wizard.direction === 'N' || undefined }), 'Northbound'),
      h('label', {}, h('input', { type: 'radio', name: 'raildir', value: 'S', checked: wizard.direction === 'S' || undefined }), 'Southbound'),
      h('label', {}, h('input', { type: 'radio', name: 'raildir', value: '', checked: wizard.direction === '' || wizard.direction == null || undefined }), 'Both directions'));
    card.append(h('p', { class: 'small muted' }, '“Northbound/Southbound” are SEPTA’s legacy division names, not compass directions.'));
    card.append(choices);
    card.append(h('div', { class: 'row', style: 'margin-top:.6rem' },
      h('button', { onclick: () => { wizard.step = 'station'; drawStops(); } }, 'Back'),
      h('button', { class: 'primary', onclick: () => {
        const picked = choices.querySelector('input[name="raildir"]:checked');
        wizard.direction = picked ? picked.value : '';
        wizard.dirToken = wizard.direction === 'N' ? 'N' : wizard.direction === 'S' ? 'S' : 'both';
        wizard.step = 'rail-details';
        drawStops();
      } }, 'Next')));
    return card;
  }

  if (wizard.step === 'rail-details') {
    card.append(wizardStepsBar('details', ['mode', 'station', 'direction', 'details']));
    const dirLabel = wizard.direction === 'N' ? 'Northbound' : wizard.direction === 'S' ? 'Southbound' : 'Both directions';
    const key = `rail-${slugify(wizard.station)}-${wizard.dirToken}`;
    const dup = liveConfig.stops.some((s) => s.key === key);
    const labelInput = h('input', { type: 'text', id: 'rail-label', value: `${wizard.station} (${dirLabel})` });
    const lineInput = h('input', { type: 'text', id: 'rail-line', placeholder: 'e.g. Paoli/Thorndale (optional)' });
    const showInput = h('select', { id: 'rail-show' }, ...[1, 2, 3, 4].map((n) => h('option', { value: n, selected: n === 2 || undefined }, `${n} row${n === 1 ? '' : 's'}`)));
    card.append(h('label', { for: 'rail-label' }, 'Label'), labelInput);
    card.append(h('label', { for: 'rail-line' }, 'Line filter (optional)'), lineInput);
    card.append(h('label', { for: 'rail-show' }, 'Rows to show'), showInput);
    if (dup) card.append(h('div', { class: 'banner warn' }, 'This station and direction is already configured.'));
    const err = h('div', {});
    card.append(err);
    card.append(h('div', { class: 'row', style: 'margin-top:.6rem' },
      h('button', { onclick: () => { wizard.step = 'rail-direction'; drawStops(); } }, 'Back'),
      h('button', {
        class: 'primary', disabled: dup,
        onclick: async () => {
          const stop = {
            key, mode: 'rail', route: '', stop_id: '', station: wizard.station,
            direction: wizard.direction, line: lineInput.value.trim(), headsign: '',
            label: labelInput.value.trim() || key, stop_name: wizard.station,
            show: Number(showInput.value),
          };
          const next = { ...liveConfig, stops: [...liveConfig.stops, stop] };
          try {
            const saved = await api.saveConfig(next);
            liveConfig = saved || next;
            wizard = null;
            drawStops();
            stopsBanner('Stop added.', 'ok');
          } catch (e) {
            clear(err); err.append(h('div', { class: 'field-error' }, e.path ? `${e.message} (${e.path})` : e.message));
          }
        },
      }, 'Save stop')));
    return card;
  }

  return card;
}

function modeButton(mode, title, desc) {
  return h('button', { onclick: () => {
    wizard.mode = mode;
    wizard.step = mode === 'rail' ? 'station' : 'route';
    drawStops();
  } }, h('strong', {}, title), h('span', { class: 'small muted' }, desc));
}

/* ======================== Stats view ======================== */

let statsSelection = { stop: null, days: 30 };
// Guards async continuations the same way stopsActive does for the Stops view.
let statsActive = false;

async function renderStats(root) {
  statsActive = true;
  activeCleanup = () => { statsActive = false; };
  root.append(h('p', { class: 'muted' }, 'Loading…'));
  let cfg;
  try { cfg = await api.config(); } catch (e) { if (!statsActive) return; clear(root); root.append(h('div', { class: 'banner danger' }, e.message)); return; }
  if (!statsActive) return;
  clear(root);
  if (!cfg.stops.length) {
    root.append(h('div', { class: 'card muted' }, 'Add a stop first, then come back here for its statistics.'));
    return;
  }
  if (!statsSelection.stop || !cfg.stops.some((s) => s.key === statsSelection.stop)) statsSelection.stop = cfg.stops[0].key;

  const stopSelect = h('select', { id: 'stats-stop' }, ...cfg.stops.map((s) => h('option', { value: s.key, selected: s.key === statsSelection.stop || undefined }, s.label)));
  const daysSelect = h('select', { id: 'stats-days' }, ...[7, 30, 90].map((n) => h('option', { value: n, selected: n === statsSelection.days || undefined }, `${n} days`)));
  const controls = h('div', { class: 'card row' },
    h('label', { for: 'stats-stop', class: 'inline' }, 'Stop'), stopSelect,
    h('label', { for: 'stats-days', class: 'inline' }, 'Window'), daysSelect);
  const body = h('div', { class: 'stack' }, h('p', { class: 'muted' }, 'Loading statistics…'));
  const csvCard = h('div', { class: 'card' }, h('h3', {}, 'Download logs'), h('div', { id: 'csv-list' }, 'Loading…'));
  root.append(controls, body, csvCard);

  async function load() {
    clear(body);
    body.append(h('p', { class: 'muted' }, 'Loading statistics…'));
    try {
      const data = await api.stats(statsSelection.stop, statsSelection.days);
      if (!statsActive) return;
      clear(body);
      body.append(buildStatsBody(data));
    } catch (e) {
      if (!statsActive) return;
      clear(body);
      body.append(h('div', { class: 'banner danger' }, e.message));
    }
  }
  stopSelect.addEventListener('change', () => { statsSelection.stop = stopSelect.value; load(); });
  daysSelect.addEventListener('change', () => { statsSelection.days = Number(daysSelect.value); load(); });
  load();

  api.logIndex().then((files) => {
    if (!statsActive) return;
    const box = $('#csv-list');
    if (!box) return;
    clear(box);
    if (!files || !files.length) { box.append(h('p', { class: 'muted small' }, 'No log files yet.')); return; }
    const ul = h('ul', {});
    for (const f of files) ul.append(h('li', {}, h('a', { href: `/api/log/${encodeURIComponent(f.file)}` }, f.file), ` — ${humanBytes(f.bytes)}`));
    box.append(ul);
  }).catch(() => {
    if (!statsActive) return;
    const box = $('#csv-list');
    if (box) { clear(box); box.append(h('p', { class: 'muted small' }, 'Could not load log index.')); }
  });
}

function buildStatsBody(data) {
  const wrap = h('div', { class: 'stack' });
  if (!data || !data.samples) {
    wrap.append(h('div', { class: 'card muted' }, 'No data yet for this stop and window. Statistics build up once the device has been logging arrivals for a while.'));
    return wrap;
  }
  const tiles = h('div', { class: 'card tiles' },
    statTile('On time', `${data.on_time_pct?.toFixed?.(1) ?? data.on_time_pct}%`),
    statTile('Mean late', `${data.mean_late_min} min`),
    statTile('Samples', data.samples),
    statTile('Ghosts', data.ghost ?? 0),
    statTile('No-shows', data.noshow ?? 0),
    statTile('Outage min', data.outage_min ?? 0));
  wrap.append(tiles);

  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Lateness by hour'), chartWrap(buildBarChart(fillHours(data.by_hour), (s, i) => (i % 3 === 0 ? s.h : ''))), chartLegendMinutes()));
  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Lateness by weekday'), chartWrap(buildBarChart(fillWeekdays(data.by_weekday), (s) => ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'][s.d])), chartLegendMinutes()));

  const hw = data.headway || {};
  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Headway'),
    h('div', { class: 'tiles', style: 'margin-bottom:.6rem' },
      statTile('Samples', hw.n ?? 0), statTile('Bunched', hw.bunched ?? 0), statTile('Gapped', hw.gapped ?? 0)),
    chartWrap(buildHistChart(hw.ratio_hist)),
    h('div', { class: 'chart-legend' },
      h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-bunched)' }), 'Bunched (<40% of scheduled gap)'),
      h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-normal)' }), 'Normal'),
      h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-gapped)' }), 'Gapped (>175%)'))));

  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Prediction accuracy by horizon'), chartWrap(buildPredictionChart(data.prediction || [])),
    h('p', { class: 'small muted' }, 'Bar = mean absolute error in seconds. Dot above/below = bias (red = predictions ran late, blue = predictions ran early).')));

  return wrap;
}

function statTile(k, v) { return h('div', { class: 'tile' }, h('div', { class: 'k' }, k), h('div', { class: 'v' }, v)); }
function chartWrap(svg) { return h('div', { class: 'chart-wrap' }, svg); }
function chartLegendMinutes() {
  return h('div', { class: 'chart-legend' },
    h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-normal)' }), '≤5 min mean late'),
    h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-gapped)' }), '5–15 min'),
    h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-bunched)' }), '>15 min'),
    h('span', {}, '│ whisker = p50–p90'));
}

function fillHours(arr) {
  const slots = Array.from({ length: 24 }, (_, hh) => ({ h: hh, n: 0, mean: 0, p50: 0, p90: 0 }));
  for (const e of arr || []) { if (e.h >= 0 && e.h < 24) slots[e.h] = { h: e.h, n: e.n || 0, mean: e.mean || 0, p50: e.p50 || 0, p90: e.p90 || 0 }; }
  return slots;
}
function fillWeekdays(arr) {
  const slots = Array.from({ length: 7 }, (_, d) => ({ d, n: 0, mean: 0, p50: 0, p90: 0 }));
  for (const e of arr || []) {
    const d = e.d ?? e.weekday ?? e.day ?? e.wd;
    if (d >= 0 && d < 7) slots[d] = { d, n: e.n || 0, mean: e.mean || 0, p50: e.p50 || 0, p90: e.p90 || 0 };
  }
  return slots;
}

function buildBarChart(slots, labelFor) {
  const w = Math.max(320, slots.length * 26);
  const hgt = 150, padL = 26, padB = 20, padT = 8;
  const chartH = hgt - padT - padB;
  const maxV = Math.max(1, ...slots.map((s) => Math.max(s.mean, s.p90, 1)));
  const barW = (w - padL) / slots.length;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Lateness chart' });
  const step = Math.ceil(maxV / 4) || 1;
  for (let gv = 0; gv <= maxV; gv += step) {
    const y = padT + chartH - (gv / maxV) * chartH;
    svg.append(hs('line', { x1: padL, x2: w, y1: y, y2: y, stroke: 'var(--border)', 'stroke-width': 1 }));
    svg.append(hs('text', { x: 1, y: y + 3, 'font-size': 9, fill: 'var(--text-muted)' }, String(gv)));
  }
  slots.forEach((s, i) => {
    const x = padL + i * barW + 1;
    const bw = Math.max(2, barW - 2);
    const barH = (s.mean / maxV) * chartH;
    const y = padT + chartH - barH;
    const color = s.n === 0 ? 'var(--border)' : s.mean > 15 ? 'var(--chip-bunched)' : s.mean > 5 ? 'var(--chip-gapped)' : 'var(--chip-normal)';
    svg.append(hs('rect', { x, y, width: bw, height: Math.max(0, barH), fill: color }));
    if (s.n > 0 && s.p90 > 0) {
      const yp50 = padT + chartH - (s.p50 / maxV) * chartH;
      const yp90 = padT + chartH - (s.p90 / maxV) * chartH;
      const cx = x + bw / 2;
      svg.append(hs('line', { x1: cx, x2: cx, y1: yp50, y2: yp90, stroke: 'var(--text)', 'stroke-width': 1.5 }));
      svg.append(hs('line', { x1: cx - 3, x2: cx + 3, y1: yp90, y2: yp90, stroke: 'var(--text)', 'stroke-width': 1.5 }));
    }
    const lbl = labelFor(s, i);
    if (lbl !== '') svg.append(hs('text', { x: x + bw / 2, y: hgt - 4, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, String(lbl)));
  });
  return svg;
}

function normalizeHist(hist) {
  return (hist || []).map((item, i) => {
    if (typeof item === 'number') return { label: `#${i + 1}`, count: item };
    const count = item.count ?? item.n ?? 0;
    const label = item.bucket ?? item.label ?? (item.pct_lo != null ? `${item.pct_lo}-${item.pct_hi ?? ''}%` : `#${i + 1}`);
    return { label, count, pctLo: item.pct_lo, pctHi: item.pct_hi };
  });
}

function buildHistChart(hist) {
  const bins = normalizeHist(hist);
  if (!bins.length) return h('p', { class: 'muted small' }, 'No headway samples yet.');
  const w = Math.max(320, bins.length * 40);
  const hgt = 140, padB = 26, padT = 8;
  const chartH = hgt - padT - padB;
  const maxV = Math.max(1, ...bins.map((b) => b.count));
  const barW = w / bins.length;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Headway ratio histogram' });
  bins.forEach((b, i) => {
    const x = i * barW + 3;
    const bw = Math.max(2, barW - 6);
    const barH = (b.count / maxV) * chartH;
    const y = padT + chartH - barH;
    let color = 'var(--accent)';
    if (b.pctHi != null) color = b.pctHi <= 40 ? 'var(--chip-bunched)' : b.pctLo >= 175 ? 'var(--chip-gapped)' : 'var(--chip-normal)';
    svg.append(hs('rect', { x, y, width: bw, height: Math.max(0, barH), fill: color }));
    svg.append(hs('text', { x: x + bw / 2, y: hgt - 12, 'font-size': 8, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, b.label));
    svg.append(hs('text', { x: x + bw / 2, y: hgt - 2, 'font-size': 8, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, String(b.count)));
  });
  return svg;
}

function buildPredictionChart(pred) {
  if (!pred.length) return h('p', { class: 'muted small' }, 'No prediction samples yet.');
  const w = Math.max(280, pred.length * 90);
  const hgt = 160, padT = 30, padB = 24;
  const chartH = hgt - padT - padB;
  const maxMae = Math.max(1, ...pred.map((p) => p.mae_s || 0));
  const maxBias = Math.max(1, ...pred.map((p) => Math.abs(p.bias_s || 0)));
  const barW = w / pred.length;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Prediction accuracy by horizon' });
  const mid = padT + chartH / 2;
  svg.append(hs('line', { x1: 0, x2: w, y1: mid, y2: mid, stroke: 'var(--border)' }));
  pred.forEach((p, i) => {
    const x = i * barW + 8;
    const bw = Math.max(10, barW - 16);
    const barH = (p.mae_s / maxMae) * (chartH / 2);
    svg.append(hs('rect', { x, y: mid - barH, width: bw, height: barH, fill: 'var(--accent)' }));
    const bias = p.bias_s || 0;
    const by = mid - barH - 8 - (bias / maxBias) * (chartH / 2 - 10);
    svg.append(hs('circle', { cx: x + bw / 2, cy: Math.max(padT, Math.min(hgt - padB, by)), r: 4, fill: bias >= 0 ? 'var(--chip-bunched)' : 'var(--chip-normal)' }));
    const mins = Math.round(p.horizon_s / 60);
    svg.append(hs('text', { x: x + bw / 2, y: hgt - 6, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, `${mins}m (n=${p.n})`));
    svg.append(hs('text', { x: x + bw / 2, y: mid - barH - 14, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, `${p.mae_s}s`));
  });
  return svg;
}

/* ======================== Settings view ======================== */

let settingsActive = false;

async function renderSettings(root) {
  settingsActive = true;
  activeCleanup = () => { settingsActive = false; };
  root.append(h('p', { class: 'muted' }, 'Loading…'));
  let cfg, state;
  try {
    [cfg, state] = await Promise.all([api.config(), api.state().catch(() => ({}))]);
  } catch (e) { if (!settingsActive) return; clear(root); root.append(h('div', { class: 'banner danger' }, e.message)); return; }
  if (!settingsActive) return;
  clear(root);

  const banner = h('div', {});
  root.append(banner);

  const d = cfg.device;
  const nameInput = h('input', { type: 'text', id: 'set-name', value: d.name });
  const mdnsPreview = h('span', { class: 'muted small' }, `${slugify(d.name)}.local`);
  nameInput.addEventListener('input', () => { mdnsPreview.textContent = `${slugify(nameInput.value)}.local`; });

  const tzSelect = h('select', { id: 'set-tz' },
    ...US_TIMEZONES.map(([label, tz]) => h('option', { value: tz, selected: tz === d.tz || undefined }, label)),
    h('option', { value: '__custom__', selected: !US_TIMEZONES.some(([, tz]) => tz === d.tz) || undefined }, 'Custom…'));
  const tzCustom = h('input', { type: 'text', id: 'set-tz-custom', value: d.tz, placeholder: 'POSIX TZ string' });
  tzCustom.classList.toggle('hidden', tzSelect.value !== '__custom__');
  tzSelect.addEventListener('change', () => tzCustom.classList.toggle('hidden', tzSelect.value !== '__custom__'));

  const pollInput = h('input', { type: 'number', id: 'set-poll', min: 15, max: 120, value: d.poll_seconds });
  const ROTATIONS = [[0, 'Portrait (0°)'], [90, 'Landscape (90°)'], [180, 'Portrait, flipped (180°)'], [270, 'Landscape, flipped (270°)']];
  const rotSelect = h('select', { id: 'set-rotation' },
    ...ROTATIONS.map(([deg, label]) => h('option', { value: String(deg), selected: deg === (d.rotation ?? 0) || undefined }, label)));
  const brightInput = h('input', { type: 'range', id: 'set-bright', min: 10, max: 100, value: d.brightness });
  const brightVal = h('span', { class: 'small muted' }, `${d.brightness}%`);
  brightInput.addEventListener('input', () => { brightVal.textContent = `${brightInput.value}%`; });
  const httpsInput = h('input', { type: 'checkbox', id: 'set-https' });
  httpsInput.checked = !!d.use_https;
  const tlsInput = h('input', { type: 'checkbox', id: 'set-tls' });
  tlsInput.checked = d.tls_verify;
  const loggingInput = h('input', { type: 'checkbox', id: 'set-logging' });
  loggingInput.checked = d.logging;
  const alertsInput = h('input', { type: 'checkbox', id: 'set-alerts' });
  alertsInput.checked = cfg.alerts;

  const tlsWarn = h('div', { class: `banner warn${tlsInput.checked ? ' hidden' : ''}` },
    'Disabling certificate verification lets a device on the network impersonate SEPTA and send fake arrival times. Only turn this off for troubleshooting.');
  tlsInput.addEventListener('change', () => tlsWarn.classList.toggle('hidden', tlsInput.checked));

  const form = h('div', { class: 'card settings-grid' },
    h('h2', {}, 'Device'),
    h('label', { for: 'set-name' }, 'Device name'), h('div', { class: 'row' }, nameInput, mdnsPreview),
    h('label', { for: 'set-tz' }, 'Timezone'), tzSelect, tzCustom,
    h('label', { for: 'set-poll' }, 'Poll interval (seconds)'), pollInput,
    h('label', { for: 'set-rotation' }, 'Screen rotation'), rotSelect,
    h('label', { for: 'set-bright' }, 'Screen brightness'), h('div', { class: 'row' }, brightInput, brightVal),
    h('label', { class: 'inline', style: 'margin-top:1rem' }, httpsInput, ' Fetch SEPTA data over HTTPS'),
    h('p', { class: 'small muted' }, 'Off by default: a TLS session needs about 40 KB of RAM the classic ESP32 does not have to spare. SEPTA serves the same data over plain HTTP.'),
    h('label', { class: 'inline' }, tlsInput, ' Verify SEPTA’s TLS certificate (when HTTPS is on)'), tlsWarn,
    h('label', { class: 'inline' }, loggingInput, ' Log arrivals to SD card'),
    h('label', { class: 'inline' }, alertsInput, ' Show service alerts'),
    h('div', { style: 'margin-top:1rem' }, h('button', { class: 'primary', onclick: async () => {
      const tz = tzSelect.value === '__custom__' ? tzCustom.value.trim() : tzSelect.value;
      const next = {
        ...cfg,
        device: {
          ...d, name: nameInput.value.trim(), tz, poll_seconds: Number(pollInput.value),
          brightness: Number(brightInput.value), rotation: Number(rotSelect.value), use_https: httpsInput.checked, tls_verify: tlsInput.checked, logging: loggingInput.checked,
        },
        alerts: alertsInput.checked,
      };
      clear(banner);
      try {
        await api.saveConfig(next);
        cfg = next;
        banner.append(h('div', { class: 'banner ok', role: 'status' }, 'Settings saved.'));
      } catch (e) {
        banner.append(h('div', { class: 'banner danger', role: 'alert' }, e.path ? `${e.message} (${e.path})` : e.message));
      }
    } }, 'Save settings')));
  root.append(form);

  root.append(h('div', { class: 'card' }, h('h2', {}, 'Firmware'),
    h('p', {}, 'Version: ', h('strong', {}, state.firmware_version || 'unknown')),
    h('label', { for: 'ota-file' }, 'Upload new firmware (.bin)'),
    h('input', { type: 'file', id: 'ota-file', accept: '.bin' }),
    h('div', { id: 'progress-wrap' }, h('div', { id: 'progress-bar' })),
    h('div', { id: 'ota-status', class: 'small muted' }),
    h('button', { class: 'primary', style: 'margin-top:.6rem', onclick: async (ev) => {
      const fileInput = $('#ota-file');
      const status = $('#ota-status');
      const bar = $('#progress-bar');
      if (!fileInput.files.length) { status.textContent = 'Choose a .bin file first.'; return; }
      if (!confirm('Upload and install this firmware? The device will reboot when done.')) return;
      ev.target.disabled = true;
      status.textContent = 'Uploading…';
      try {
        await uploadFirmware(fileInput.files[0], (frac) => { bar.style.width = `${Math.round(frac * 100)}%`; });
        status.textContent = 'Upload complete. Device is installing and will reboot.';
      } catch (e) {
        status.textContent = e.message;
      } finally {
        ev.target.disabled = false;
      }
    } }, 'Upload')));

  root.append(h('div', { class: 'card' }, h('h2', {}, 'Maintenance'),
    h('div', { class: 'row' },
      h('button', { onclick: async () => {
        if (!confirm('Reset Wi-Fi credentials? The device will start its setup access point again.')) return;
        try { await api.wifiReset(); alert('Wi-Fi reset requested.'); } catch (e) { alert(e.message); }
      } }, 'Reset Wi-Fi'),
      h('button', { class: 'danger', onclick: async () => {
        if (!confirm('Reboot the device now?')) return;
        try { await api.reboot(); alert('Reboot requested.'); } catch (e) { alert(e.message); }
      } }, 'Reboot device'))));
}

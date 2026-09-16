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

// One or two plain sentences under a control, saying what it actually does. The owner is
// not a programmer; a setting with no explanation is a setting nobody dares touch.
function hint(...text) { return h('p', { class: 'hint' }, ...text); }

// Hints used in more than one place (the add wizard and the edit form show the same
// fields), kept together so the wording cannot drift apart.
const HINT_SHOW_ROWS = 'How many upcoming arrivals to list for this stop. The screen may '
  + 'fit fewer than you ask for when several stops share it.';
const HINT_TITLE_STYLE = 'How this stop is titled on the device’s screen. “Label → '
  + 'destination” reads “17 Southbound → 20th-Johnston”; “Label only” reads “17 '
  + 'Southbound”; “Route → destination • stop” reads “17 → 20th-Johnston • 19th St & '
  + 'Mifflin St”; “Custom text” shows exactly what you type.';
const HINT_ALT_OF = 'Normally every stop is always on screen. Pick another stop here and '
  + 'this one hides itself until that stop has nothing coming within the minutes below — '
  + 'useful for a backup route you only want to see when your usual one is a long way off.';
const HINT_RAIL_LINE = 'Leave this on “Any line” to see every train at this station going '
  + 'that way, or pick one line to show only its trains. SEPTA identifies lines by the '
  + 'three-letter code shown here.';

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
function fmtClock(epoch) {
  const d = new Date(epoch * 1000);
  const h = d.getHours() % 12 || 12;
  return `${h}:${String(d.getMinutes()).padStart(2, '0')}${d.getHours() < 12 ? 'a' : 'p'}`;
}
// Minutes until the arrival; from an hour out, the clock time instead (a scheduled trip hours
// away - overnight service, or SEPTA answering with the wrong service day - reads as "1:14a",
// not "958"). Mirrors ui_common.cpp etaLabel() on the device.
function fmtEta(a) {
  const etaS = a.eta_s;
  if (etaS == null) return '--';
  if (etaS <= 0) return 'Now';
  if (etaS < 60) return 'Due';
  const when = a.predicted || a.scheduled;
  if (etaS >= 3600 && when) return fmtClock(when);
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
function haversineMiles(lat1, lng1, lat2, lng2) {
  const R = 3958.8;
  const toRad = (d) => (d * Math.PI) / 180;
  const dLat = toRad(lat2 - lat1);
  const dLng = toRad(lng2 - lng1);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(dLng / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

/* ======================== API layer (DESIGN.md §7) ======================== */

class ApiError extends Error {
  constructor(message, path, status, retryS) {
    super(message);
    this.path = path;
    this.status = status;
    this.retry_s = retryS;
  }
}

/* ---- Admin PIN.
   The device asks for a PIN before anything that changes it (PUT /api/config,
   POST /api/reboot | /api/wifi/reset | /api/ota | /api/pin) and before handing out the
   CSV logs. It travels in an `X-Pin` header; a missing or wrong PIN is 401, and five
   wrong tries in a row is 429 with `retry_s`. The PIN is remembered per browser in
   localStorage so the owner types it once, not on every save. */

const PIN_KEY = 'ptd_pin';
const PIN_HELP = 'This device asks for its PIN before changing settings. Find it on the '
  + 'device: tap the top of the screen to open the device info page, or read it from the '
  + 'serial console.';

const pinStore = {
  // Private-mode Safari throws on localStorage; treating that as "no PIN saved" just
  // means the modal asks every time rather than the page failing outright.
  get() { try { return localStorage.getItem(PIN_KEY) || ''; } catch (e) { return ''; } },
  set(v) { try { localStorage.setItem(PIN_KEY, v); } catch (e) {} },
  clear() { try { localStorage.removeItem(PIN_KEY); } catch (e) {} },
  has() { return !!pinStore.get(); },
};

function withPin(opts) {
  const next = { ...(opts || {}) };
  next.headers = { ...(next.headers || {}) };
  const pin = pinStore.get();
  if (pin) next.headers['X-Pin'] = pin;
  return next;
}

// "too many attempts" carries retry_s; say the wait in words rather than echoing a number
// with no unit.
function retryMessage(body) {
  const s = Number(body && body.retry_s);
  const wait = !s || !isFinite(s) ? 'a little while'
    : s >= 60 ? `${Math.ceil(s / 60)} minute${Math.ceil(s / 60) === 1 ? '' : 's'}`
    : `${Math.round(s)} seconds`;
  return `Too many wrong PIN tries. The device is refusing new ones for ${wait}.`;
}

// Modal PIN prompt. Resolves with the PIN typed, or null when dismissed. Only one can be
// open at a time; concurrent callers (two saves racing) share the same promise.
let pinPromptOpen = null;
function askForPin(message) {
  if (pinPromptOpen) return pinPromptOpen;
  pinPromptOpen = new Promise((resolve) => {
    const input = h('input', { type: 'password', id: 'pin-input', autocomplete: 'current-password' });
    const err = h('div', {});
    let done = false;
    const finish = (value) => {
      if (done) return;
      done = true;
      document.removeEventListener('keydown', onKey);
      backdrop.remove();
      pinPromptOpen = null;
      resolve(value);
    };
    const submit = () => {
      const v = input.value.trim();
      if (!v) { clear(err); err.append(h('div', { class: 'field-error' }, 'Enter the PIN.')); return; }
      finish(v);
    };
    const onKey = (ev) => { if (ev.key === 'Escape') finish(null); };
    const dialog = h('div', { class: 'modal', role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': 'pin-modal-title' },
      h('h2', { id: 'pin-modal-title' }, 'Device PIN'),
      h('p', { class: 'small' }, message || PIN_HELP),
      h('label', { for: 'pin-input' }, 'PIN'), input, err,
      h('div', { class: 'row', style: 'margin-top:.8rem' },
        h('button', { class: 'primary', onclick: submit }, 'Unlock'),
        h('button', { onclick: () => finish(null) }, 'Cancel')));
    input.addEventListener('keydown', (ev) => { if (ev.key === 'Enter') { ev.preventDefault(); submit(); } });
    const backdrop = h('div', { class: 'modal-backdrop', onclick: (ev) => { if (ev.target === backdrop) finish(null); } }, dialog);
    document.addEventListener('keydown', onKey);
    document.body.append(backdrop);
    input.focus();
  });
  return pinPromptOpen;
}

async function rawFetchJSON(url, opts, timeoutMs) {
  let res;
  // Only the heavy, memory-gated reads (GET /api/state, GET /api/config) pass a
  // timeout: a device that's overloaded enough to hang mid-response should be treated
  // the same as one that answered 503, not left to hang the page forever. Everything
  // else (writes, proxy calls, stats) keeps the browser's own default.
  const controller = timeoutMs ? new AbortController() : null;
  const timer = controller ? setTimeout(() => controller.abort(), timeoutMs) : null;
  try {
    res = await fetch(url, controller ? { ...(opts || {}), signal: controller.signal } : opts);
  } catch (e) {
    throw new ApiError('Network error: could not reach the device.', null, 0);
  } finally {
    if (timer) clearTimeout(timer);
  }
  let body = null;
  const ct = res.headers.get('content-type') || '';
  if (ct.includes('application/json')) {
    try { body = await res.json(); } catch (e) { /* ignore parse failure -- treated like a missing body below */ }
  }
  if (!res.ok) {
    const msg = res.status === 429 ? retryMessage(body)
      : (body && body.error) || `Request failed (HTTP ${res.status})`;
    throw new ApiError(msg, body && body.path, res.status, body && body.retry_s);
  }
  return body;
}

// needsPin marks the protected calls: send the saved PIN, and on 401 ask for it and
// replay the request once. A 429 is surfaced as-is — retrying would only extend the wait.
async function fetchJSON(url, opts, needsPin) {
  if (!needsPin) return rawFetchJSON(url, opts);
  try {
    return await rawFetchJSON(url, withPin(opts));
  } catch (e) {
    if (!(e instanceof ApiError) || e.status !== 401) throw e;
    const wrong = /wrong pin/i.test(e.message || '');
    const pin = await askForPin(wrong ? `That PIN was not accepted. ${PIN_HELP}` : PIN_HELP);
    if (!pin) throw new ApiError('This device needs its PIN before it will accept that change.', null, 401);
    pinStore.set(pin);
    // Only one retry: a second 401 means the PIN itself is wrong, and saying so beats
    // echoing the firmware's two-word "wrong pin".
    return rawFetchJSON(url, withPin(opts)).catch((e2) => { throw asPinError(e2); });
  }
}

// Turns a repeat 401 into something the owner can act on.
function asPinError(e) {
  if (e instanceof ApiError && e.status === 401) {
    return new ApiError(`That PIN was not accepted. ${PIN_HELP}`, null, 401);
  }
  return e;
}

// PUT /api/config is memory-gated the same way the heavy reads are (DESIGN.md §12.1)
// and can answer 503 too. Unlike a read, a write is never retried automatically — the
// device is telling us it's busy building the *next* thing, and stacking retried saves
// on top would be the eager-retry mistake this project has already crashed a device
// with once. So: say it calmly, not as a raw "Request failed (HTTP 503)", and leave the
// edit exactly where it was so a second press of Save is all it takes. Shared by
// Settings' save button and the Stops page's own PUT /api/config (reorder/add/remove)
// so the wording can't drift between the two the way HINT_* keeps hint text in sync.
function writeErrorMessage(e) {
  if (e instanceof ApiError && e.status === 503) {
    return { cls: 'warn', text: 'The display is busy right now and couldn’t save. Nothing was changed — try again in a moment.' };
  }
  return { cls: 'danger', text: e.path ? `${e.message} (${e.path})` : e.message };
}

/* ---- Resilient reads: GET /api/state and GET /api/config (DESIGN.md §12.1).
   These two "heavy" endpoints deliberately refuse with a 503 when the device is short
   on heap mid-poll, and /api/state can additionally come back as a genuine HTTP 200
   with a zero-length body (the network stack's send buffer, not an application error).
   Roughly a third to a half of requests can be refused during a poll cycle. None of
   that is a real error: it clears in a second or two, and the documented contract is
   "retry." A 503, an empty 200, a body that fails to parse, a network error and a
   timeout are folded into the exact same "busy" outcome here, so every caller (the Now
   page's poll, Settings' load) sees one simple thing: the promise resolves a bit late
   sometimes, and never rejects for a reason the owner needs to see.

   The retry loop is deliberately open-ended (capped, jittered backoff, no give-up) —
   the device is healthy and this clears on its own, so stopping would just bring back
   the blank/stuck page this exists to prevent. `inflightReads` keeps at most one
   request per endpoint in flight app-wide: a second caller (the next poll tick, or a
   different view wanting the same data) joins the retry already running instead of
   starting its own, which matters because eager retries are exactly what amplifies the
   memory pressure they're retrying against. */
const HEAVY_TIMEOUT_MS = 8000;
const RETRY_BASE_MS = 1000;
const RETRY_MAX_MS = 8000;
const RETRY_JITTER = 0.4; // +/- 40%, so several open tabs don't retry in lockstep

function jitteredDelay(attempt) {
  const base = Math.min(RETRY_BASE_MS * 2 ** (attempt - 1), RETRY_MAX_MS);
  const spread = base * RETRY_JITTER;
  return Math.round(base - spread + Math.random() * spread * 2);
}
function sleep(ms) { return new Promise((resolve) => setTimeout(resolve, ms)); }

// One entry per watched endpoint: whether it's currently being refused, how many
// attempts in a row, and when it last actually succeeded (for "how stale" wording).
// `connListeners` gets a call after every change so the top-bar indicator (see the
// bottom of this file) can repaint without each view wiring up its own polling.
const connStatus = {
  state: { busy: false, attempt: 0, lastGoodAt: null },
  config: { busy: false, attempt: 0, lastGoodAt: null },
};
const connListeners = new Set();
function watchConn(fn) { connListeners.add(fn); fn(); return () => connListeners.delete(fn); }
function notifyConn() { for (const fn of connListeners) fn(); }
function markBusy(key, attempt) { connStatus[key].busy = true; connStatus[key].attempt = attempt; notifyConn(); }
function markGood(key) { connStatus[key].busy = false; connStatus[key].attempt = 0; connStatus[key].lastGoodAt = Date.now(); notifyConn(); }

const inflightReads = new Map();
function resilientRead(key, url) {
  const existing = inflightReads.get(key);
  if (existing) return existing;
  const promise = (async () => {
    let attempt = 0;
    for (;;) {
      try {
        const body = await rawFetchJSON(url, undefined, HEAVY_TIMEOUT_MS);
        // A successful-looking response with nothing in it is the documented empty-200:
        // treat it exactly like a 503 rather than handing callers a null "state".
        if (body == null) throw new ApiError('The display is busy right now.', null, 503);
        markGood(key);
        return body;
      } catch (e) {
        attempt++;
        markBusy(key, attempt);
        await sleep(jitteredDelay(attempt));
      }
    }
  })();
  inflightReads.set(key, promise);
  promise.finally(() => { if (inflightReads.get(key) === promise) inflightReads.delete(key); });
  return promise;
}

const api = {
  state: () => resilientRead('state', '/api/state'),
  config: () => resilientRead('config', '/api/config'),
  saveConfig: (cfg) => fetchJSON('/api/config', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(cfg),
  }, true),
  proxyStops: (route) => fetchJSON(`/api/proxy/stops?route=${encodeURIComponent(route)}`),
  proxySchedule: (stopId) => fetchJSON(`/api/proxy/schedule?stop_id=${encodeURIComponent(stopId)}`),
  railStations: () => fetchJSON('/api/rail/stations'),
  stats: (stop, days) => fetchJSON(`/api/stats?stop=${encodeURIComponent(stop)}&days=${encodeURIComponent(days)}`),
  statsOverview: (days) => fetchJSON(`/api/stats/overview?days=${encodeURIComponent(days)}`),
  logIndex: () => fetchJSON('/api/log/index'),
  reboot: () => fetchJSON('/api/reboot', { method: 'POST' }, true),
  wifiReset: () => fetchJSON('/api/wifi/reset', { method: 'POST' }, true),
  setPin: (pin) => fetchJSON('/api/pin', {
    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ pin }),
  }, true),
};

// The CSV logs are PIN-protected, so a bare <a href> would land the owner on a 401 JSON
// page. Fetch the bytes with the header instead and hand them to the browser through a
// Blob + object URL + synthetic click, which is the only way to attach a header to a
// download.
async function downloadLog(file) {
  const url = `/api/log/${encodeURIComponent(file)}`;
  const once = async () => {
    let res;
    try { res = await fetch(url, withPin({})); }
    catch (e) { throw new ApiError('Network error: could not reach the device.', null, 0); }
    if (!res.ok) {
      let body = null;
      try { body = await res.json(); } catch (e) {}
      const msg = res.status === 429 ? retryMessage(body)
        : (body && body.error) || `Download failed (HTTP ${res.status})`;
      throw new ApiError(msg, null, res.status, body && body.retry_s);
    }
    return res.blob();
  };
  let blob;
  try {
    blob = await once();
  } catch (e) {
    if (!(e instanceof ApiError) || e.status !== 401) throw e;
    const pin = await askForPin(PIN_HELP);
    if (!pin) throw new ApiError('This device needs its PIN before it will hand over the logs.', null, 401);
    pinStore.set(pin);
    blob = await once().catch((e2) => { throw asPinError(e2); });
  }
  const href = URL.createObjectURL(blob);
  const a = h('a', { href, download: file });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(href), 10000);
}

function uploadFirmware(file, onProgress) {
  const send = (pin) => new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    if (pin) xhr.setRequestHeader('X-Pin', pin);
    xhr.upload.onprogress = (e) => { if (e.lengthComputable) onProgress(e.loaded / e.total); };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) { resolve(); return; }
      let body = null;
      try { body = JSON.parse(xhr.responseText); } catch (e) {}
      const detail = (body && body.error) || '';
      let msg = detail || `Update failed (HTTP ${xhr.status})`;
      if (xhr.status === 409) msg = 'The device is already installing an update. Wait for it to finish and reboot, then try again.';
      else if (xhr.status === 429) msg = retryMessage(body);
      else if (xhr.status === 400) msg = `The device would not take this file${detail ? `: ${detail}` : '.'}`;
      reject(new ApiError(msg, null, xhr.status, body && body.retry_s));
    };
    xhr.onerror = () => reject(new ApiError('Network error during upload.', null, 0));
    const fd = new FormData();
    fd.append('firmware', file, file.name);
    xhr.send(fd);
  });
  // A 401 costs a re-upload of the whole image; unavoidable, the header has to be on the
  // request that carries the body.
  return send(pinStore.get()).catch(async (e) => {
    if (!(e instanceof ApiError) || e.status !== 401) throw e;
    const pin = await askForPin(PIN_HELP);
    if (!pin) throw new ApiError('This device needs its PIN before it will install firmware.', null, 401);
    pinStore.set(pin);
    return send(pin).catch((e2) => { throw asPinError(e2); });
  });
}

/* ======================== Leaflet loader (CDN, graceful degrade) ======================== */

// Pinned to 1.9.4 and checked with Subresource Integrity, so a compromised or
// impersonated CDN cannot inject script into the page that holds the device's PIN.
// A hash mismatch makes the browser refuse the file, the loader times out, and the
// wizard falls back to its plain stop list — the same path as being offline.
//
// Hashes computed from the exact pinned files with:
//   curl -sL https://unpkg.com/leaflet@1.9.4/dist/leaflet.css \
//     | openssl dgst -sha384 -binary | openssl base64 -A
//   curl -sL https://unpkg.com/leaflet@1.9.4/dist/leaflet.js \
//     | openssl dgst -sha384 -binary | openssl base64 -A
// (2026-09-15; leaflet.css 14806 B, leaflet.js 147552 B). crossorigin="anonymous" is
// required or the browser cannot read the bytes to check them.
const LEAFLET_CSS_URL = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';
const LEAFLET_CSS_SRI = 'sha384-sHL9NAb7lN7rfvG5lfHpm643Xkcjzp4jFvuavGOndn6pjVqS6ny56CAt3nsEVT4H';
const LEAFLET_JS_URL = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
const LEAFLET_JS_SRI = 'sha384-cxOPjt7s7Iz04uaHJceBmS+qpjv2JkIHNVcuOrM+YHwZOmJGBXI00mdUXEq65HTH';

let leafletPromise = null;
function loadLeaflet() {
  if (leafletPromise) return leafletPromise;
  leafletPromise = new Promise((resolve) => {
    if (window.L) { resolve(window.L); return; }
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = LEAFLET_CSS_URL;
    link.integrity = LEAFLET_CSS_SRI;
    link.crossOrigin = 'anonymous';
    document.head.appendChild(link);
    const script = document.createElement('script');
    script.src = LEAFLET_JS_URL;
    script.integrity = LEAFLET_JS_SRI;
    script.crossOrigin = 'anonymous';
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

/* ======================== top-bar "device is busy" indicator ======================== */
// One line, shared by every view, reflecting whatever resilientRead() is currently
// retrying (see the API layer above). It says so quietly and disappears on its own the
// moment a read succeeds — no page has to wire this up itself.
(() => {
  const bar = $('#conn-status');
  if (!bar) return;
  watchConn(() => {
    const busyState = connStatus.state.busy;
    const busyConfig = connStatus.config.busy;
    if (!busyState && !busyConfig) { bar.classList.remove('show'); bar.textContent = ''; return; }
    const parts = [];
    if (busyState) {
      const age = connStatus.state.lastGoodAt ? fmtAgo((Date.now() - connStatus.state.lastGoodAt) / 1000) : null;
      parts.push(age ? `live data (last update ${age})` : 'live data');
    }
    if (busyConfig) parts.push('settings');
    bar.textContent = `The display is busy right now — retrying ${parts.join(' and ')}…`;
    bar.classList.add('show');
  });
})();

/* ======================== Now view ======================== */

function renderNow(root) {
  const wrap = h('div', { class: 'stack' });
  const banner = h('div', { id: 'now-banner' });
  const strip = h('div', { id: 'status-strip', class: 'card muted' }, 'Connecting to the display…');
  const profileLine = h('div', { id: 'now-profile' });
  const stops = h('div', { id: 'now-stops', class: 'stack' });
  const bike = h('div', { id: 'now-bike' });
  const alerts = h('div', { id: 'now-alerts', class: 'card hidden' },
    h('h2', {}, 'Service alerts'), h('div', { id: 'alerts-list' }));
  wrap.append(banner, strip, profileLine, stops, bike, alerts);
  root.append(wrap);

  let stopped = false;
  // Fetched once (not per poll tick); every tick awaits this promise, so it's ready by
  // the first render. Falls back to words/seats on fetch failure or old firmware.
  let crowdMode = 'words';
  let crowdScheme = 'seats';
  // bike.style is config-only (not echoed on /api/state); old firmware that doesn't
  // know the field yet is treated as 'icons', its default.
  let bikeStyle = 'icons';
  const configPromise = api.config().then((cfg) => {
    const cd = (cfg && cfg.device) || {};
    crowdMode = cd.crowding || (cd.show_crowding === false ? 'off' : 'words');
    crowdScheme = cd.crowding_icons || 'seats';
    bikeStyle = (cfg && cfg.bike && cfg.bike.style === 'words') ? 'words' : 'icons';
  }).catch(() => {});

  async function tick() {
    if (stopped) return;
    try {
      await configPromise;
      if (stopped) return;
      const state = await api.state();
      if (stopped) return;
      renderStatusStrip(strip, state);
      renderStaleBanner(banner, state);
      renderProfileLine(profileLine, state);
      renderStopPanels(stops, state.stops || [], crowdMode, crowdScheme);
      renderBikeCard(bike, state.bike, bikeStyle);
      renderAlerts(alerts, state.alerts || []);
    } catch (e) {
      // api.state()/api.config() already absorb the documented 503/empty-200/network/
      // timeout cases into a silent retry (see resilientRead above) and the top-bar
      // indicator says so — this only fires for something genuinely unexpected, so it's
      // fine for it to look like a real error. Whatever was already on screen (from the
      // last successful tick) is left exactly as it was; a stop's panel only ever
      // reflects data this page actually received.
      if (stopped) return;
      clear(banner);
      banner.append(h('div', { class: 'banner danger', role: 'alert' }, e.message || 'Could not load state.'));
    }
  }
  tick();
  const timer = setInterval(tick, 15000);
  activeCleanup = () => { stopped = true; clearInterval(timer); };
}

function renderProfileLine(box, state) {
  clear(box);
  if (state.active_profile) box.append(h('div', { class: 'muted small' }, `Profile: ${state.active_profile}`));
}

// ---- Indego card (Now view): bike.style icons/words picks how each station's
// counts are drawn; state.bike doesn't carry style itself (config-only), see bikeStyle
// above. Original 14x14 pictograms below, drawn the same way as crowdIcon() — not
// traced from any icon set.
function bikeGlyph(kind) {
  if (kind === 'bike') {
    return [
      hs('circle', { cx: 3.6, cy: 10.4, r: 2.5, fill: 'none', stroke: 'currentColor', 'stroke-width': 1.3 }),
      hs('circle', { cx: 10.4, cy: 10.4, r: 2.5, fill: 'none', stroke: 'currentColor', 'stroke-width': 1.3 }),
      hs('polyline', { points: '3.6,10.4 6.7,4.7 10.4,10.4', fill: 'none', stroke: 'currentColor', 'stroke-width': 1.3, 'stroke-linejoin': 'round' }),
      hs('line', { x1: 6.7, y1: 4.7, x2: 7, y2: 3.2, stroke: 'currentColor', 'stroke-width': 1.3 }),
      hs('line', { x1: 6.7, y1: 4.7, x2: 5.4, y2: 10.4, stroke: 'currentColor', 'stroke-width': 1.3 }),
    ];
  }
  if (kind === 'bolt') {
    return [hs('polygon', { points: '7.6,1 2.6,8.4 5.8,8.4 5.1,13 11.1,6.1 7.8,6.1', fill: 'currentColor' })];
  }
  // dock: a simple "P"-in-a-rounded-square.
  return [
    hs('rect', { x: 1.2, y: 1.2, width: 11.6, height: 11.6, rx: 3, fill: 'none', stroke: 'currentColor', 'stroke-width': 1.3 }),
    hs('text', { x: 7, y: 10.2, 'text-anchor': 'middle', 'font-size': 8.5, 'font-weight': 700, fill: 'currentColor', 'font-family': 'sans-serif' }, 'P'),
  ];
}
function bikeIcon(kind) {
  return hs('svg', { viewBox: '0 0 14 14', width: 13, height: 13, 'aria-hidden': 'true' }, ...bikeGlyph(kind));
}
// red at 0, amber at 1-2, normal text color otherwise (null -> no class).
function bikeToneClass(n) {
  if (n === 0) return 'tone-red';
  if (n === 1 || n === 2) return 'tone-amber';
  return null;
}
function bikeAgeLabel(age_s) {
  if (age_s == null || age_s <= 600) return null;
  return `${Math.round(age_s / 60)} min old`;
}
function bikeCountsIcons(st) {
  const wrap = h('span', { class: 'bike-counts' });
  for (const [kind, n] of [['bike', st.classic], ['bolt', st.ebikes], ['dock', st.docks]]) {
    const tone = bikeToneClass(n);
    wrap.append(h('span', { class: tone ? `bike-count ${tone}` : 'bike-count' }, bikeIcon(kind), String(n)));
  }
  return wrap;
}
function bikeCountsWords(st) {
  const words = [
    [st.classic, `bike${st.classic === 1 ? '' : 's'}`],
    [st.ebikes, `e-bike${st.ebikes === 1 ? '' : 's'}`],
    [st.docks, `dock${st.docks === 1 ? '' : 's'}`],
  ];
  const wrap = h('span', { class: 'bike-counts words small' });
  words.forEach(([n, word], i) => {
    if (i) wrap.append(', ');
    wrap.append(h('span', { class: bikeToneClass(n) }, `${n} ${word}`));
  });
  return wrap;
}
function renderStationRow(st, style) {
  const nameEl = h('span', { class: 'bike-name' }, st.name);
  if (st.active === false) return h('div', { class: 'bike-row' }, nameEl, h('span', { class: 'muted small' }, 'offline'));
  if (st.bikes === -1) return h('div', { class: 'bike-row' }, nameEl, h('span', { class: 'muted small' }, 'no data'));
  return h('div', { class: 'bike-row' }, nameEl, style === 'words' ? bikeCountsWords(st) : bikeCountsIcons(st));
}
function renderBikeCard(box, bike, style) {
  clear(box);
  if (!bike || !bike.enabled || !bike.stations || !bike.stations.length) return;
  const ageLabel = bikeAgeLabel(bike.age_s);
  // Same container/title-bar classes as a stop panel (.card.stop-panel / .title) so this
  // card reads as one more panel in the stack, not a visually distinct widget; "title row
  // between" reuses the existing .row.between flex utility to right-align the stale note
  // inside that same bar instead of stacking it below.
  const titleBar = h('div', { class: 'title row between' },
    h('span', { class: 'bike-title' }, bikeIcon('bike'), 'Indego'),
    ageLabel && h('span', { class: 'small bike-age-warn' }, ageLabel));
  const card = h('div', { class: 'card stop-panel' }, titleBar);
  for (const st of bike.stations) card.append(renderStationRow(st, style === 'words' ? 'words' : 'icons'));
  box.append(card);
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
  const wx = state.weather || {};
  if (wx.enabled && wx.main) {
    tiles.splice(0, 0, ['Weather', `${Math.round(wx.main.temp)}°${(wx.units || 'f').toUpperCase()} ${wx.main.text || ''}`.trim()]);
  }
  // Which transport the arrivals actually came over (DESIGN.md §2.1). Only firmware built with
  // HTTPS reports a `transport` block; an older build gets no tile rather than a wrong one.
  if (state.transport) tiles.push(['Data link', describeTransport(state.transport)]);
  for (const [k, v] of tiles) {
    strip.append(h('div', { class: 'status-tile' }, h('div', { class: 'k' }, k), h('div', { class: 'v' }, v)));
  }
}

// The `transport` block of /api/state in one short phrase. `last` is the most recent fetch that
// asked for https://: the device chooses TLS or plain HTTP by its free memory (never because a
// certificate looked wrong - a bad certificate is a failed fetch, not a downgrade), so the
// answer can change from one poll to the next and the tile says which it was.
function describeTransport(tr) {
  const policy = tr.policy || 'http';
  switch (tr.last) {
    case 'https': return `Encrypted (HTTPS)${tr.last_https_ms ? `, ${(tr.last_https_ms / 1000).toFixed(1)} s to connect` : ''}`;
    case 'http': return policy === 'http' ? 'Plain HTTP (by setting)' : 'Plain HTTP this time (not enough free memory for HTTPS)';
    case 'https_failed': return tr.cert_failed ? 'HTTPS failed: certificate did not verify (not fetched over plain HTTP)' : 'HTTPS failed: could not connect (not fetched over plain HTTP)';
    case 'refused': return 'Waiting for free memory (HTTPS required, plain HTTP not allowed)';
    default: return policy === 'http' ? 'Plain HTTP (by setting)' : 'Not fetched yet';
  }
}

// The badge answers two separate questions and must not conflate them: `status` says
// whether the device has a live prediction for this trip, `late_known` says whether SEPTA
// also told it how late the trip is running. A live trip with no lateness figure is still
// live — labelling it "sched / no live tracking" was wrong (review F31).
function badgeFor(a) {
  const status = a.status || (a.late_known ? 'live' : 'unknown');
  if (status === 'skipped') return { cls: 'skip', text: 'skipped', aria: 'SEPTA says this trip skips this stop' };
  if (status === 'scheduled') return { cls: 'sched', text: 'sched', aria: 'Scheduled time only — this trip is not being tracked live' };
  if (status !== 'live') return { cls: 'sched', text: 'sched', aria: 'No live tracking for this trip' };
  if (!a.late_known) return { cls: 'live', text: 'live', aria: 'live ETA, lateness unknown' };
  const late = a.late_min || 0;
  if (late > 5) return { cls: 'late', text: `+${late}`, aria: `${late} minutes late` };
  if (late < -1) return { cls: 'early', text: `−${Math.abs(late)}`, aria: `${Math.abs(late)} minutes early` };
  return { cls: 'on-time', text: 'on time', aria: 'On time' };
}

// Per-stop feed health. The firmware sends `health`, `source_ts` and `source_age_s` on each
// /api/state stop; older builds send none of it, in which case nothing is drawn. `live` is
// the normal case and needs no chip — a badge on every panel all the time would be noise.
function stopHealthChip(s) {
  const health = s.health;
  if (!health || health === 'live') return null;
  if (health === 'stale') {
    // source_age_s is how old SEPTA's own data is; fall back to source_ts if only that came.
    const ageS = s.source_age_s != null ? s.source_age_s
      : s.source_ts ? Math.max(0, Math.floor(Date.now() / 1000) - s.source_ts) : null;
    const mins = ageS == null ? null : Math.max(1, Math.round(ageS / 60));
    const text = mins == null ? 'stale' : `stale ${mins} min`;
    return h('span', { class: 'stop-health stale', title: 'The live feed for this stop has not refreshed recently; these times are the last ones SEPTA gave.' }, text);
  }
  if (health === 'unavailable') {
    return h('span', { class: 'stop-health unavailable', title: 'The device could not reach the live feed for this stop.' },
      s.error ? `unavailable: ${s.error}` : 'unavailable');
  }
  if (health === 'schedule_only') {
    return h('span', { class: 'stop-health schedule-only', title: 'No live tracking here — these are timetable times.' }, 'schedule only');
  }
  return h('span', { class: 'stop-health schedule-only' }, String(health));
}

// ---- Crowding: device.crowding off/words/icons/both, device.crowding_icons seats/crowd.
// arrivals[].seats is SEPTA's raw estimated_seat_availability string; word mapping below
// must match the firmware exactly. Unrecognized/blank seats -> no element.
const CROWD_WORDS = {
  EMPTY: 'empty', MANY_SEATS_AVAILABLE: 'open', FEW_SEATS_AVAILABLE: 'few seats',
  STANDING_ROOM_ONLY: 'standing', CRUSHED_STANDING_ROOM_ONLY: 'packed', FULL: 'full',
};
// Three-slot icon recipes, left to right: [glyph, tone]. 'dim' keeps unused slots visible.
const CROWD_ICONS_SEATS = {
  EMPTY: [['chair', 'green'], ['chair', 'green'], ['chair', 'green']],
  MANY_SEATS_AVAILABLE: [['chair', 'green'], ['chair', 'green'], ['chair', 'dim']],
  FEW_SEATS_AVAILABLE: [['chair', 'amber'], ['chair', 'dim'], ['chair', 'dim']],
  STANDING_ROOM_ONLY: [['person', 'amber'], ['person', 'dim'], ['person', 'dim']],
  CRUSHED_STANDING_ROOM_ONLY: [['person', 'red'], ['person', 'red'], ['person', 'dim']],
  FULL: [['person', 'red'], ['person', 'red'], ['person', 'red']],
};
const CROWD_ICONS_CROWD = {
  EMPTY: [['person', 'green'], ['person', 'dim'], ['person', 'dim']],
  MANY_SEATS_AVAILABLE: [['person', 'green'], ['person', 'dim'], ['person', 'dim']],
  FEW_SEATS_AVAILABLE: [['person', 'amber'], ['person', 'amber'], ['person', 'dim']],
  STANDING_ROOM_ONLY: [['person', 'amber'], ['person', 'amber'], ['person', 'dim']],
  CRUSHED_STANDING_ROOM_ONLY: [['person', 'red'], ['person', 'red'], ['person', 'red']],
  FULL: [['person', 'red'], ['person', 'red'], ['person', 'red']],
};
// Original 14x14 pictograms (not traced from any icon set).
const CROWD_TONE_VAR = { green: '--on-time-fg', amber: '--warn', red: '--late-fg', dim: '--text-muted' };
function crowdIcon(kind, tone) {
  const parts = kind === 'chair'
    ? [hs('rect', { x: 2, y: 1, width: 2, height: 8 }), hs('rect', { x: 2, y: 7, width: 9, height: 2 }),
       hs('rect', { x: 2, y: 9, width: 2, height: 4 }), hs('rect', { x: 9, y: 9, width: 2, height: 4 })]
    : [hs('circle', { cx: 7, cy: 3, r: 2.1 }), hs('rect', { x: 4, y: 6.3, width: 6, height: 7.2, rx: 2.6 })];
  return hs('svg', {
    viewBox: '0 0 14 14', width: 13, height: 13, fill: 'currentColor', 'aria-hidden': 'true',
    style: `color:var(${CROWD_TONE_VAR[tone]})`,
  }, ...parts);
}
function crowdElement(seats, mode, scheme) {
  if (!mode || mode === 'off') return null;
  const word = CROWD_WORDS[seats];
  if (!word) return null;
  const label = `Crowding: ${word}`;
  const span = h('span', { class: 'arrival-crowd', title: label, 'aria-label': label });
  if (mode === 'icons' || mode === 'both') {
    const recipe = (scheme === 'crowd' ? CROWD_ICONS_CROWD : CROWD_ICONS_SEATS)[seats] || [];
    span.append(h('span', { class: 'crowd-icons' }, ...recipe.map(([k, t]) => crowdIcon(k, t))));
  }
  if (mode === 'words' || mode === 'both') span.append(h('span', { class: 'small muted' }, word));
  return span;
}

function renderStopPanels(container, stopSnaps, crowdMode, crowdScheme) {
  clear(container);
  if (!stopSnaps.length) {
    container.append(h('div', { class: 'card muted' }, 'No stops configured yet. Add one from the Stops tab.'));
    return;
  }
  for (const s of stopSnaps) {
    const title = `${s.route || (s.mode === 'rail' ? 'Rail' : '')}${s.headsign ? ' → ' + s.headsign : ''}${s.stop_name ? ' · ' + s.stop_name : ''}`.trim()
      || s.label || s.key;
    const chip = stopHealthChip(s);
    const panel = h('div', { class: 'card stop-panel' },
      h('div', { class: 'title row between' },
        h('span', {}, s.label ? `${s.label} — ${title}` : title), chip));
    if (s.weather_note) panel.append(h('div', { class: 'small', style: 'color:var(--early-fg)' }, s.weather_note));
    // `ok` is false for a stale stop as well as an unreachable one, but stale still has
    // usable (if old) times — hiding them behind a red banner would throw away the only
    // information the panel has. Only a genuinely empty stop gets the banner; stale keeps
    // its rows, with the amber chip in the title bar saying how old they are.
    const stale = s.health === 'stale';
    const hasRows = s.arrivals && s.arrivals.length;
    if (!s.ok && !(stale && hasRows)) {
      panel.append(h('div', { class: 'banner danger' }, s.error || 'No data for this stop.'));
    } else if (!hasRows) {
      panel.append(h('div', { class: 'muted small' }, 'No upcoming arrivals.'));
    } else {
      if (stale && s.error) panel.append(h('div', { class: 'small muted' }, s.error));
      for (const a of s.arrivals.slice(0, s.show || 4)) {
        const badge = badgeFor(a);
        panel.append(h('div', { class: 'arrival-row' },
          h('span', { class: 'route-badge' }, s.route || (s.mode === 'rail' ? 'RR' : '?')),
          h('span', { class: 'arrival-dest' }, a.destination || s.headsign || ''),
          crowdElement(a.seats, crowdMode, crowdScheme),
          h('span', { class: 'arrival-minutes' }, fmtEta(a)),
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

// Stops saved before this version carry no coordinates, so they get no weather. The Stops API
// the wizard already proxies has them; one click fills them in and saves.
function coordinatesBanner() {
  const missing = (liveConfig.stops || []).filter((s) => s.mode !== 'rail' && !(Number(s.lat) && Number(s.lng)));
  if (!missing.length) return null;
  const btn = h('button', { onclick: async (ev) => {
    ev.target.disabled = true;
    try {
      const routes = [...new Set(missing.map((s) => s.route))];
      const byId = new Map();
      for (const r of routes) {
        const list = await api.proxyStops(r).catch(() => []);
        for (const e of list || []) byId.set(String(e.stopid), e);
      }
      let filled = 0;
      const next = { ...liveConfig, stops: liveConfig.stops.map((s) => {
        const e = byId.get(String(s.stop_id));
        if (!e || s.mode === 'rail' || (Number(s.lat) && Number(s.lng))) return s;
        filled++;
        return { ...s, lat: Number(e.lat) || 0, lng: Number(e.lng) || 0 };
      }) };
      if (!filled) { stopsBanner('SEPTA did not list those stops on their routes; coordinates not found.', 'warn'); return; }
      liveConfig = await api.saveConfig(next) || next;
      drawStops();
      stopsBanner(`Added coordinates for ${filled} stop${filled === 1 ? '' : 's'}; weather will follow on the next poll.`, 'ok');
    } catch (e) {
      stopsBanner(e.message);
    } finally {
      ev.target.disabled = false;
    }
  } }, 'Look up coordinates');
  return h('div', { class: 'banner warn' },
    `${missing.length} stop${missing.length === 1 ? ' has' : 's have'} no coordinates, so ${missing.length === 1 ? 'it gets' : 'they get'} no weather. `, btn);
}

function drawStops() {
  clear(stopsRoot);
  const wrap = h('div', { class: 'stack' });
  wrap.append(h('div', { id: 'stops-banner' }));
  if (!wizard) {
    const cb = coordinatesBanner();
    if (cb) wrap.append(cb);
  }
  if (wizard) {
    wrap.append(drawWizard());
  } else {
    wrap.append(drawStopList());
    wrap.append(drawBikeCard());
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

/* ---- Regional Rail line filter (review F30).
   The firmware filters rail arrivals by SEPTA's line *code* ("PAO"), not the public name
   ("Paoli/Thorndale"), so the old free-text box quietly produced a filter that never
   matched. The code is what gets stored; the name is only ever a label in this list. */
const RAIL_LINES = [
  ['AIR', 'Airport'],
  ['CHE', 'Chestnut Hill East'],
  ['CHW', 'Chestnut Hill West'],
  ['CYN', 'Cynwyd'],
  ['FOX', 'Fox Chase'],
  ['LAN', 'Lansdale/Doylestown'],
  ['MED', 'Media/Wawa'],
  ['NOR', 'Manayunk/Norristown'],
  ['PAO', 'Paoli/Thorndale'],
  ['TRE', 'Trenton'],
  ['WAR', 'Warminster'],
  ['WIL', 'Wilmington/Newark'],
  ['WTR', 'West Trenton'],
];

// Stops saved by an earlier version hold a display name (or nothing) where the code
// belongs. Map whatever is there onto a code so the dropdown opens on the right entry;
// anything unrecognised falls back to "Any line" rather than inventing a filter.
function railLineCode(value) {
  const v = String(value == null ? '' : value).trim();
  if (!v) return '';
  const upper = v.toUpperCase();
  if (RAIL_LINES.some(([code]) => code === upper)) return upper;
  const byName = RAIL_LINES.find(([, name]) => name.toLowerCase() === v.toLowerCase());
  return byName ? byName[0] : '';
}

// A rail stop's line filter, wherever an older config happened to put it.
function railLineOf(s) {
  return railLineCode(s.route) || railLineCode(s.line);
}

function railLineSelect(id, current) {
  const code = railLineCode(current);
  return h('select', { id },
    h('option', { value: '', selected: !code || undefined }, 'Any line'),
    ...RAIL_LINES.map(([c, name]) => h('option', { value: c, selected: c === code || undefined }, `${c} — ${name}`)));
}

/* ---- Bus vs trolley (review F30).
   SEPTA's own mode list has `trolley` as a separate mode and the firmware treats it as
   one, but the wizard's single "Bus / Trolley" button always saved `bus`. The route id
   is a good guess — City/subway-surface trolleys and the two Media/Sharon Hill lines
   have known ids — but it is only a default the user can override. */
const TROLLEY_ROUTE_RE = /^(T\d|G1|D\d|10|11|13|15|34|36|101|102)$/i;
function looksLikeTrolley(route) {
  return TROLLEY_ROUTE_RE.test(String(route || '').trim());
}
const SURFACE_MODE_HINT = 'SEPTA counts trolleys as their own mode, so the device asks for '
  + 'them differently. Routes T1–T5, G1, D1, 10, 11, 13, 15, 34, 36, 101 and 102 are '
  + 'trolleys; everything else on the street is a bus.';

function surfaceModeSelect(id, current) {
  return h('select', { id },
    h('option', { value: 'bus', selected: current !== 'trolley' || undefined }, 'Bus'),
    h('option', { value: 'trolley', selected: current === 'trolley' || undefined }, 'Trolley'));
}

// Options for stops[].title_style (DESIGN.md §6, "Fields added 2026-09-14").
const TITLE_STYLES = [
  ['label_dest', 'Label → destination'],
  ['label', 'Label only'],
  ['route_dest_stop', 'Route → destination • stop'],
  ['custom', 'Custom text'],
];

function directionWord(s) {
  return s.direction === 'N' ? 'Northbound' : s.direction === 'S' ? 'Southbound' : 'Both';
}

// Preview of the on-screen title for a stop, mirroring how the firmware will render it
// per DESIGN.md §6 "Fields added 2026-09-14".
function stopTitlePreview(s) {
  const style = s.title_style || 'label_dest';
  if (style === 'custom') return s.title_text || '';
  if (style === 'label') return s.label || (s.mode === 'rail' ? s.station : s.route) || s.key;
  if (s.mode === 'rail') {
    if (style === 'route_dest_stop') return `${railLineOf(s) || 'Regional Rail'} → ${directionWord(s)} • ${s.station || ''}`;
    return `${s.label || 'Regional Rail'} (${directionWord(s)})`;
  }
  if (style === 'route_dest_stop') return `${s.route || ''} → ${s.headsign || ''} • ${s.stop_name || ''}`;
  return `${s.label || s.route || ''} → ${s.headsign || ''}`;
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
    const altTarget = s.alt_of ? liveConfig.stops.find((o) => o.key === s.alt_of) : null;
    list.append(h('div', { class: 'stop-list-item' },
      h('div', { class: 'meta' },
        h('div', { class: 'name' }, stopTitle(s)),
        h('div', { class: 'small muted' }, `key: ${s.key} · shows ${s.show} row${s.show === 1 ? '' : 's'}`),
        h('div', { class: 'small muted' }, `On screen: “${stopTitlePreview(s)}”`),
        altTarget && h('div', { class: 'small muted' }, `alternative to ${altTarget.label} after ${s.alt_after_min ?? 15} min`)),
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

  const titleStyleSelect = h('select', { id: `edit-title-style-${i}` },
    ...TITLE_STYLES.map(([v, lbl]) => h('option', { value: v, selected: (s.title_style || 'label_dest') === v || undefined }, lbl)));
  const titleTextInput = h('input', { type: 'text', id: `edit-title-text-${i}`, maxlength: 40, value: s.title_text || s.label || '' });
  const titleTextWrap = h('div', { class: (s.title_style || 'label_dest') === 'custom' ? '' : 'hidden' },
    h('label', { for: `edit-title-text-${i}` }, 'Custom text'), titleTextInput);
  titleStyleSelect.addEventListener('change', () => titleTextWrap.classList.toggle('hidden', titleStyleSelect.value !== 'custom'));

  const otherStops = liveConfig.stops.filter((o) => o.key !== s.key);
  const altOfSelect = h('select', { id: `edit-alt-of-${i}` },
    h('option', { value: '', selected: !s.alt_of || undefined }, 'Always show (not an alternative)'),
    ...otherStops.map((o) => h('option', { value: o.key, selected: s.alt_of === o.key || undefined }, o.label)));
  const altAfterInput = h('input', { type: 'number', id: `edit-alt-after-${i}`, min: 5, max: 60, value: s.alt_after_min ?? 15, disabled: !s.alt_of });
  altOfSelect.addEventListener('change', () => { altAfterInput.disabled = !altOfSelect.value; });

  const fields = [
    h('label', { for: `edit-label-${i}` }, 'Label'), labelInput,
    hint('Your own short name for this stop, used on screen and everywhere in this app.'),
    h('label', { for: `edit-show-${i}` }, 'Rows to show'), showInput,
    hint(HINT_SHOW_ROWS),
    h('label', { for: `edit-title-style-${i}` }, 'Title on screen'), titleStyleSelect, titleTextWrap,
    hint(HINT_TITLE_STYLE),
    h('label', { for: `edit-alt-of-${i}` }, 'Show only as an alternative to'), altOfSelect,
    hint(HINT_ALT_OF),
    h('label', { for: `edit-alt-after-${i}` }, "when that stop's next bus is more than N minutes away"), altAfterInput,
  ];
  if (s.mode === 'rail') {
    const dirSel = h('select', { id: `edit-dir-${i}` },
      h('option', { value: 'N', selected: s.direction === 'N' || undefined }, 'Northbound'),
      h('option', { value: 'S', selected: s.direction === 'S' || undefined }, 'Southbound'),
      h('option', { value: '', selected: !s.direction || undefined }, 'Both'));
    const lineSel = railLineSelect(`edit-line-${i}`, railLineOf(s));
    fields.push(h('label', { for: `edit-dir-${i}` }, 'Direction'), dirSel,
      hint('“Northbound” and “Southbound” are SEPTA’s own labels for the two halves of a through-running line, not compass directions.'));
    fields.push(h('label', { for: `edit-line-${i}` }, 'Line filter'), lineSel, hint(HINT_RAIL_LINE));
    fields._dirSel = dirSel; fields._lineSel = lineSel;
  } else if (s.mode === 'bus' || s.mode === 'trolley') {
    // Keeps an existing trolley a trolley on save, and lets a stop saved as `bus` by the
    // old wizard be corrected without deleting and re-adding it (review F30).
    const modeSel = surfaceModeSelect(`edit-mode-${i}`, s.mode);
    fields.push(h('label', { for: `edit-mode-${i}` }, 'Service type'), modeSel, hint(SURFACE_MODE_HINT));
    fields._modeSel = modeSel;
  }
  const form = h('div', { class: 'card' }, h('h3', {}, `Edit: ${s.label}`), ...fields,
    h('div', { class: 'row', style: 'margin-top:.6rem' },
      h('button', { class: 'primary', onclick: async () => {
        const updated = {
          ...s,
          label: labelInput.value.trim() || s.label,
          show: Number(showInput.value),
          title_style: titleStyleSelect.value,
          title_text: titleTextInput.value.trim(),
          alt_of: altOfSelect.value,
          alt_after_min: Number(altAfterInput.value),
        };
        if (s.mode === 'rail') {
          updated.direction = fields._dirSel.value;
          // The code goes in both fields: DESIGN.md §6 documents rail `line` as mapping
          // onto StopConfig::route, and the firmware contract for F30 asks for the code in
          // `route`. Writing the same code to both leaves no stale display name behind.
          updated.route = fields._lineSel.value;
          updated.line = fields._lineSel.value;
        } else if (fields._modeSel) {
          updated.mode = fields._modeSel.value;
        }
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
    const { cls, text } = writeErrorMessage(e);
    stopsBanner(text, cls);
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

/* ---- Indego bikes (top-level bike, DESIGN.md §6) ---- */
// Lives on the Stops page (managed alongside the stops it's placed near, not in Settings).
// Every action here saves immediately through persistConfig, same as moveStop/removeStop above.

function drawBikeCard() {
  const bike = liveConfig.bike || { enabled: false, stations: [] };
  const stations = bike.stations || [];
  const card = h('div', { class: 'card' });
  card.append(h('h2', {}, 'Indego bikes'));

  const enabledInput = h('input', { type: 'checkbox', id: 'bike-enabled' });
  enabledInput.checked = !!bike.enabled;
  enabledInput.addEventListener('change', () => {
    persistConfig({ ...liveConfig, bike: { ...bike, enabled: enabledInput.checked } });
  });
  card.append(h('label', { class: 'inline' }, enabledInput, ' Show Indego bike/dock counts'));
  card.append(hint('Adds a panel to the display showing how many bikes and free docks are '
    + 'waiting at up to three Indego stations, refreshed alongside the arrivals.'));

  // bike.style: icons | words (default icons for old firmware/config that predates the field).
  const styleSelect = h('select', { id: 'bike-style' },
    h('option', { value: 'icons', selected: (bike.style || 'icons') !== 'words' || undefined }, 'Icons + numbers'),
    h('option', { value: 'words', selected: bike.style === 'words' || undefined }, 'Words'));
  styleSelect.addEventListener('change', () => {
    persistConfig({ ...liveConfig, bike: { ...bike, style: styleSelect.value } });
  });
  card.append(h('label', { for: 'bike-style' }, 'Indego style'));
  card.append(styleSelect);
  card.append(hint('“Icons + numbers” draws a bicycle for classic bikes, a lightning bolt '
    + 'for e-bikes and a P for free docks. “Words” spells it out instead: “5 bikes, 2 '
    + 'e-bikes, 7 docks”. Either way a count turns red at zero and amber at one or two, so '
    + 'you can see at a glance whether it is worth walking over.'));

  const listDiv = h('div', {});
  if (!stations.length) { listDiv.append(h('p', { class: 'small muted' }, 'No stations chosen yet.')); }
  for (const st of stations) {
    listDiv.append(h('div', { class: 'row between' },
      h('span', {}, `${st.name} (#${st.id})`),
      h('button', {
        class: 'icon danger', title: 'Remove', 'aria-label': `Remove ${st.name}`,
        onclick: () => {
          const next = stations.filter((s) => s.id !== st.id);
          persistConfig({ ...liveConfig, bike: { ...bike, stations: next } });
        },
      }, '✕')));
  }
  if (stations.length >= 3) listDiv.append(h('p', { class: 'small muted' }, 'Maximum of 3 stations.'));
  card.append(listDiv);

  const nearbyResults = h('div', {});
  const findBtn = h('button', {
    onclick: async (ev) => {
      ev.target.disabled = true;
      clear(nearbyResults);
      nearbyResults.append(h('p', { class: 'muted small' }, 'Looking up nearby stations…'));
      try {
        const withCoords = (liveConfig.stops || []).filter((s) => Number(s.lat) && Number(s.lng));
        if (!withCoords.length) {
          clear(nearbyResults);
          nearbyResults.append(h('p', { class: 'muted small' }, 'None of your configured stops have coordinates yet — add some above first.'));
          return;
        }
        // HTTPS: the feed answers over TLS with `Access-Control-Allow-Origin: *`
        // (verified 2026-09-15), so this works whichever scheme this page was loaded
        // over, and no longer leaks the lookup to anyone on the LAN in cleartext.
        const res = await fetch('https://bts-status.bicycletransit.workers.dev/phl');
        if (!res.ok) throw new Error(`Indego feed returned HTTP ${res.status}`);
        const geo = await res.json();
        const scored = [];
        for (const f of geo.features || []) {
          const coords = f.geometry && f.geometry.coordinates;
          if (!coords) continue;
          const [lng, lat] = coords;
          let best = Infinity;
          for (const s of withCoords) best = Math.min(best, haversineMiles(Number(s.lat), Number(s.lng), lat, lng));
          scored.push({ p: f.properties, dist: best });
        }
        scored.sort((a, b) => a.dist - b.dist);
        const seen = new Set();
        const top = [];
        for (const item of scored) {
          if (!item.p || seen.has(item.p.id)) continue;
          seen.add(item.p.id);
          top.push(item);
          if (top.length >= 6) break;
        }
        clear(nearbyResults);
        if (!top.length) { nearbyResults.append(h('p', { class: 'muted small' }, 'No stations found in the feed.')); return; }
        const current = (liveConfig.bike && liveConfig.bike.stations) || [];
        for (const item of top) {
          const p = item.p;
          const already = current.some((s) => s.id === p.id);
          nearbyResults.append(h('div', { class: 'row between' },
            h('span', {}, `${p.name} — ${item.dist.toFixed(1)} mi — ${p.bikesAvailable ?? '?'} bikes, ${p.docksAvailable ?? '?'} docks`),
            h('button', {
              disabled: already || current.length >= 3,
              onclick: () => {
                const now = liveConfig.bike || { enabled: false, stations: [] };
                const next = [...(now.stations || []), { id: p.id, name: p.name }];
                persistConfig({ ...liveConfig, bike: { ...now, stations: next } });
              },
            }, already ? 'Added' : 'Add')));
        }
      } catch (e) {
        clear(nearbyResults);
        nearbyResults.append(h('div', { class: 'banner warn' },
          'Could not reach the Indego station feed. Your browser fetches it directly from Bicycle Transit, so this usually means no internet connection right now. You can still add a station by its id below. ' + (e && e.message ? `(${e.message})` : '')));
      } finally {
        ev.target.disabled = false;
      }
    },
  }, 'Find stations near my stops');
  card.append(h('div', { class: 'row', style: 'margin-top:.4rem' }, findBtn));
  card.append(nearbyResults);

  const manualId = h('input', { type: 'number', id: 'bike-manual-id', min: 1, placeholder: 'e.g. 3468' });
  const manualAdd = h('button', {
    disabled: stations.length >= 3,
    onclick: () => {
      const id = Math.trunc(Number(manualId.value));
      if (!id || id < 1) return;
      if (stations.some((s) => s.id === id) || stations.length >= 3) return;
      const next = [...stations, { id, name: `Station ${id}` }];
      persistConfig({ ...liveConfig, bike: { ...bike, stations: next } });
    },
  }, 'Add');
  card.append(h('label', { for: 'bike-manual-id', style: 'margin-top:.6rem' }, 'Or add by station id'));
  card.append(h('div', { class: 'row' }, manualId, manualAdd));
  card.append(h('p', { class: 'small muted' }, 'Station data from Bicycle Transit Systems (Indego).'));

  return card;
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
    // Bus/trolley is a real mode difference the old wizard hid (review F30). Guessed from
    // the route id as you type, until you set it yourself — after that your choice sticks.
    let modeSel = null;
    if (wizard.mode !== 'subway') {
      modeSel = surfaceModeSelect('route-mode', wizard.mode === 'trolley' ? 'trolley' : 'bus');
      modeSel.addEventListener('change', () => {
        wizard.modeTouched = true;
        wizard.mode = modeSel.value;
      });
      input.addEventListener('input', () => {
        if (wizard.modeTouched) return;
        const guess = looksLikeTrolley(input.value) ? 'trolley' : 'bus';
        modeSel.value = guess;
        wizard.mode = guess;
      });
      card.append(h('label', { for: 'route-mode' }, 'Service type'), modeSel, hint(SURFACE_MODE_HINT));
    }
    const err = h('div', {});
    card.append(err);
    card.append(h('div', { class: 'row', style: 'margin-top:.6rem' }, h('button', {
      class: 'primary',
      onclick: async () => {
        const route = input.value.trim();
        if (modeSel) wizard.mode = modeSel.value;
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
      // The attribution string is rendered as HTML by Leaflet, but it is a fixed literal
      // written here — no agency or user data reaches it.
      L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
        attribution: '&copy; OpenStreetMap contributors', maxZoom: 19,
      }).addTo(currentMap);
      for (const entry of wizard.stopResults) {
        const lat = +entry.lat, lng = +entry.lng;
        if (isNaN(lat) || isNaN(lng)) continue;
        // bindTooltip() renders a string through innerHTML. entry.stopname is agency data,
        // so hand Leaflet a node built with h() (text nodes only) instead (review F03).
        const marker = L.marker([lat, lng]).addTo(currentMap)
          .bindTooltip(h('span', {}, entry.stopname || ''));
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
    card.append(hint('Your own short name for this stop — “Home, towards Center City”, say.'));
    card.append(h('label', { for: 'detail-stopname' }, 'Stop name (shown on display)'), nameInput);
    card.append(hint('SEPTA’s name for the stop itself. Shorten it if it is too long for the screen.'));
    card.append(h('label', { for: 'detail-show' }, 'Rows to show'), showInput);
    card.append(hint(HINT_SHOW_ROWS));
    card.append(hint(`Saving as a ${wizard.mode === 'trolley' ? 'trolley' : wizard.mode === 'subway' ? 'subway' : 'bus'} stop. You can change that later from the stop list.`));
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
            lat: Number(wizard.lat) || 0, lng: Number(wizard.lng) || 0,
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
    const lineSel = railLineSelect('rail-line', wizard.line || '');
    const showInput = h('select', { id: 'rail-show' }, ...[1, 2, 3, 4].map((n) => h('option', { value: n, selected: n === 2 || undefined }, `${n} row${n === 1 ? '' : 's'}`)));
    card.append(h('label', { for: 'rail-label' }, 'Label'), labelInput);
    card.append(hint('Your own short name for this station, used on screen and in this app.'));
    card.append(h('label', { for: 'rail-line' }, 'Line filter (optional)'), lineSel);
    card.append(hint(HINT_RAIL_LINE));
    card.append(h('label', { for: 'rail-show' }, 'Rows to show'), showInput);
    card.append(hint(HINT_SHOW_ROWS));
    if (dup) card.append(h('div', { class: 'banner warn' }, 'This station and direction is already configured.'));
    const err = h('div', {});
    card.append(err);
    card.append(h('div', { class: 'row', style: 'margin-top:.6rem' },
      h('button', { onclick: () => { wizard.step = 'rail-direction'; drawStops(); } }, 'Back'),
      h('button', {
        class: 'primary', disabled: dup,
        onclick: async () => {
          // Line code in both fields — see the edit form for why.
          const lineCode = lineSel.value;
          const stop = {
            key, mode: 'rail', route: lineCode, stop_id: '', station: wizard.station,
            direction: wizard.direction, line: lineCode, headsign: '',
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
  const overview = h('div', { class: 'stack' }, h('div', { class: 'card muted' }, 'Loading overview…'));
  const body = h('div', { class: 'stack' }, h('p', { class: 'muted' }, 'Loading statistics…'));
  const csvCard = h('div', { class: 'card' }, h('h3', {}, 'Download logs'),
    hint('One CSV file per month, straight off the SD card — every arrival, ghost, no-show '
      + 'and outage the device recorded. The device asks for its PIN before handing them over.'),
    h('div', { id: 'csv-list' }, 'Loading…'));
  root.append(controls, overview, body, csvCard);

  // Row click in the overview table selects that stop in the detail picker below.
  function selectStop(key) {
    statsSelection.stop = key;
    stopSelect.value = key;
    load();
    drawOverview();
  }

  async function loadOverview() {
    clear(overview);
    overview.append(h('div', { class: 'card muted' }, 'Loading overview…'));
    try {
      const data = await api.statsOverview(statsSelection.days);
      if (!statsActive) return;
      lastOverview = data;
      drawOverview();
    } catch (e) {
      if (!statsActive) return;
      clear(overview);
      overview.append(h('div', { class: 'banner danger' }, e.message));
    }
  }
  let lastOverview = null;
  function drawOverview() {
    if (!lastOverview) return;
    clear(overview);
    overview.append(buildOverviewTable(lastOverview, cfg.stops, statsSelection.stop, selectStop));
    const bikeCards = buildBikeOverviewCards(lastOverview.bikes);
    if (bikeCards) overview.append(bikeCards);
  }

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
  stopSelect.addEventListener('change', () => { statsSelection.stop = stopSelect.value; load(); drawOverview(); });
  daysSelect.addEventListener('change', () => { statsSelection.days = Number(daysSelect.value); load(); loadOverview(); });
  load();
  loadOverview();

  api.logIndex().then((files) => {
    if (!statsActive) return;
    const box = $('#csv-list');
    if (!box) return;
    clear(box);
    if (!files || !files.length) { box.append(h('p', { class: 'muted small' }, 'No log files yet.')); return; }
    const err = h('div', {});
    const ul = h('ul', {});
    for (const f of files) {
      // A plain <a href> cannot carry the X-Pin header, so fetch the bytes and hand them
      // over as a Blob instead (see downloadLog).
      const btn = h('button', { class: 'linklike', onclick: async (ev) => {
        ev.target.disabled = true;
        clear(err);
        try { await downloadLog(f.file); }
        catch (e) { err.append(h('div', { class: 'field-error' }, e.message)); }
        finally { ev.target.disabled = false; }
      } }, f.file);
      ul.append(h('li', {}, btn, ` — ${humanBytes(f.bytes)}`));
    }
    box.append(ul, err);
  }).catch(() => {
    if (!statsActive) return;
    const box = $('#csv-list');
    if (box) { clear(box); box.append(h('p', { class: 'muted small' }, 'Could not load log index.')); }
  });
}

/* ---- Reading numbers out loud.
   The firmware distinguishes "zero" from "never found out", and the UI has to as well: a
   stop with no lateness data is not a stop that was 0% on time. Any value that can be
   absent goes through these, and every percentage is shown with the count it came from. */

function fmtCount(v) { return v == null ? 'no data' : String(v); }
function fmtPct(v, digits) { return v == null ? 'no data' : `${Number(v).toFixed(digits == null ? 1 : digits)}%`; }
function fmtMinutes(v) { return v == null ? 'no data' : `${v} min`; }

// Total arrivals in the window. `samples` is the firmware's name; `arrivals` was floated
// during the rename, so accept either rather than blanking the page over a spelling.
function arrivalCount(d) { return d.samples ?? d.arrivals ?? null; }
// How many of those arrivals SEPTA also gave a lateness figure for.
function lateKnownCount(d) { return d.late_known ?? d.late_known_n ?? null; }

// "On time: 92% of 48 with lateness data (61 arrivals)".
function onTimeSentence(d) {
  const arrivals = arrivalCount(d);
  const known = lateKnownCount(d);
  if (d.on_time_pct == null) {
    return arrivals == null
      ? 'No lateness data for this stop yet, so there is nothing to be on time against.'
      : `No lateness data — SEPTA never said how late any of the ${arrivals} arrivals ran.`;
  }
  const pct = `${Number(d.on_time_pct).toFixed(0)}%`;
  const of = known == null ? '' : ` of ${known} with lateness data`;
  const total = arrivals == null ? '' : ` (${arrivals} arrival${arrivals === 1 ? '' : 's'})`;
  return `On time: ${pct}${of}${total}.`;
}

// Coverage arrives as a fraction of the window, but tolerate a firmware that sends 0-100.
function coverageSentence(v) {
  if (v == null) return null;
  const n = Number(v);
  if (!isFinite(n)) return null;
  return `Data coverage ${(n <= 1 ? n * 100 : n).toFixed(0)}% — the share of the window the device managed to poll SEPTA successfully.`;
}

const INFERRED_NOTE = 'Arrivals are inferred from the bus disappearing from the live feed, '
  + 'not measured. Nothing on the device watches the kerb, so treat these as close '
  + 'estimates rather than exact times.';

function inferredSentence(d) {
  const n = d.inferred ?? null;
  const arrivals = arrivalCount(d);
  let s = INFERRED_NOTE;
  if (n != null) {
    const of = arrivals == null ? '' : ` of ${arrivals}`;
    s += ` ${n}${of} arrival${n === 1 ? '' : 's'} in this window were worked out that way.`;
  }
  if (d.unobserved) {
    s += ` A further ${d.unobserved} fell in a stretch when the device was not polling, so nothing is known about ${d.unobserved === 1 ? 'it' : 'them'}.`;
  }
  return s;
}

// `wait_basis` names the estimator the firmware used, so the page can say what the number
// means instead of asserting a method the device may have changed.
const WAIT_BASIS_WORDS = {
  half_mean_gap: 'half the average gap between buses',
  half_mean_headway: 'half the average gap between buses',
  mean_gap: 'the average gap between buses',
};
function waitBasisSentence(basis) {
  const words = basis ? (WAIT_BASIS_WORDS[basis] || String(basis).replace(/_/g, ' ')) : null;
  return words
    ? `Typical wait is estimated as ${words}, which is what you get if you turn up without checking.`
    : 'If you turn up without checking, your typical wait is about half the average gap between buses.';
}

/* ---- Forecast stability (was "prediction accuracy by horizon").
   The old card claimed to measure prediction *error* against a real arrival time. The
   device has no real arrival time — see INFERRED_NOTE — so what it honestly reports is how
   far the forecast was revised while the bus was in sight: `forecast_stability[]`, one
   bucket per horizon, carrying `mean_abs_revision_s` (how much it moved) and
   `mean_revision_s` (which way). Same bar-and-dot rendering as before, honest labels.

   Read defensively: the array form is what the firmware sends, but an object carrying a
   histogram, and the pre-rename `prediction[]` with `mae_s`/`bias_s`, both still draw. */
function forecastStabilityCard(data) {
  const blurb = 'How much the forecast moved between first sighting and the inferred arrival.';
  const card = h('div', { class: 'card' }, h('h3', {}, 'Forecast stability'), hint(blurb));
  const fs = data.forecast_stability;
  const buckets = Array.isArray(fs) ? fs : (fs ? (fs.by_horizon || fs.buckets || []) : (data.prediction || []));
  if (buckets.length) {
    card.append(chartWrap(buildRevisionChart(buckets)));
    card.append(h('p', { class: 'small muted' },
      'Bar = how far the forecast moved on average, in seconds, for buses first seen that far out. '
      + 'Dot = which way it usually moved (red = the bus kept slipping later, blue = it arrived sooner than first forecast).'));
    return card;
  }
  // Object form with a histogram of per-trip movement.
  const histBins = (fs && (fs.hist || fs.histogram || fs.bins)) || [];
  if (!histBins.length) {
    card.append(h('p', { class: 'muted small' }, 'No data yet.'));
    return card;
  }
  const n = fs.n ?? fs.samples ?? null;
  const typical = fs.median_s ?? fs.p50_s ?? null;
  const worst = fs.p90_s ?? fs.max_s ?? null;
  card.append(h('div', { class: 'tiles', style: 'margin-bottom:.6rem' },
    statTile('Trips', fmtCount(n), n == null ? null : 'with a forecast to compare'),
    statTile('Typical move', typical == null ? 'no data' : `${Math.round(typical)} s`),
    statTile('Worst 10%', worst == null ? 'no data' : `${Math.round(worst)} s`)));
  card.append(chartWrap(buildHistChart(histBins)));
  card.append(h('p', { class: 'small muted' },
    'Each bar counts trips; the label underneath is how far that trip’s forecast moved.'));
  return card;
}

function buildStatsBody(data) {
  const wrap = h('div', { class: 'stack' });
  const arrivals = data ? arrivalCount(data) : null;
  if (!data || !arrivals) {
    wrap.append(h('div', { class: 'card muted' }, 'No data yet for this stop and window. Statistics build up once the device has been logging arrivals for a while.'));
    return wrap;
  }
  const known = lateKnownCount(data);
  const tiles = h('div', { class: 'card' },
    h('div', { class: 'tiles' },
      // No denominator under a "no data" value — "of 0 with lateness data" reads like a
      // measurement when it is the absence of one.
      statTile('On time', fmtPct(data.on_time_pct, 0),
        data.on_time_pct == null || known == null ? null : `of ${known} with lateness data`),
      statTile('Mean late', fmtMinutes(data.mean_late_min)),
      statTile('Arrivals', fmtCount(arrivals), data.inferred == null ? null : `${data.inferred} inferred`),
      // `unobserved` sits beside `coverage`: trips the device simply was not watching for.
      // Counted separately from ghosts and no-shows, which are things it *did* watch fail.
      data.unobserved == null ? null : statTile('Unobserved', fmtCount(data.unobserved), 'device was not polling'),
      statTile('Ghosts', fmtCount(data.ghost)),
      statTile('No-shows', fmtCount(data.noshow)),
      statTile('Outage', data.outage_min == null ? 'no data' : `${data.outage_min} min`)),
    hint(onTimeSentence(data)),
    hint(inferredSentence(data)),
    coverageSentence(data.coverage) ? hint(coverageSentence(data.coverage)) : null);
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

  wrap.append(forecastStabilityCard(data));

  const crowding = data.crowding || {};
  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Crowding by hour'),
    chartWrap(buildCrowdChart(fillCrowdSlots(crowding.by_hour, 24, 'h'), (s, i) => (i % 3 === 0 ? s.h : ''))),
    crowdLegend()));
  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Crowding by weekday'),
    chartWrap(buildCrowdChart(fillCrowdSlots(crowding.by_weekday, 7, 'wd'), (s) => ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'][s.wd])),
    crowdLegend()));

  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Expected wait by hour'), chartWrap(buildWaitChart(fillWaitHours(data.wait_by_hour))),
    h('p', { class: 'small muted' }, 'Bar = average gap between buses, in minutes. Tick = worst gap seen.'),
    hint(waitBasisSentence(data.wait_basis))));

  wrap.append(h('div', { class: 'card' }, h('h3', {}, 'Reliability by hour'), chartWrap(buildReliabilityChart(fillWaitHours(data.wait_by_hour))),
    h('div', { class: 'chart-legend' },
      h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-bunched)' }), 'Ghosts (vanished before arriving)'),
      h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-gapped)' }), 'No-shows (scheduled, never came)'))));

  return wrap;
}

// `note` carries the sample count a percentage came from, so a number is never shown
// without the evidence behind it.
function statTile(k, v, note) {
  return h('div', { class: 'tile' }, h('div', { class: 'k' }, k), h('div', { class: 'v' }, v),
    note ? h('div', { class: 'tile-note' }, note) : null);
}
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

// forecast_stability[] buckets: mean_abs_revision_s (bar, how far the forecast moved) and
// mean_revision_s (dot, which way). `mae_s`/`bias_s` are the pre-rename spellings and are
// still accepted so an older device does not draw an empty chart.
function revisionSize(p) { return p.mean_abs_revision_s ?? p.mae_s ?? 0; }
function revisionBias(p) { return p.mean_revision_s ?? p.bias_s ?? 0; }

function buildRevisionChart(pred) {
  if (!pred.length) return h('p', { class: 'muted small' }, 'No forecast samples yet.');
  const w = Math.max(280, pred.length * 90);
  const hgt = 160, padT = 30, padB = 24;
  const chartH = hgt - padT - padB;
  const maxSize = Math.max(1, ...pred.map((p) => revisionSize(p)));
  const maxBias = Math.max(1, ...pred.map((p) => Math.abs(revisionBias(p))));
  const barW = w / pred.length;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Forecast stability by horizon' });
  const mid = padT + chartH / 2;
  svg.append(hs('line', { x1: 0, x2: w, y1: mid, y2: mid, stroke: 'var(--border)' }));
  pred.forEach((p, i) => {
    const x = i * barW + 8;
    const bw = Math.max(10, barW - 16);
    const size = revisionSize(p);
    const barH = (size / maxSize) * (chartH / 2);
    svg.append(hs('rect', { x, y: mid - barH, width: bw, height: barH, fill: 'var(--accent)' }));
    const bias = revisionBias(p);
    const by = mid - barH - 8 - (bias / maxBias) * (chartH / 2 - 10);
    svg.append(hs('circle', { cx: x + bw / 2, cy: Math.max(padT, Math.min(hgt - padB, by)), r: 4, fill: bias >= 0 ? 'var(--chip-bunched)' : 'var(--chip-normal)' }));
    const mins = Math.round((p.horizon_s || 0) / 60);
    svg.append(hs('text', { x: x + bw / 2, y: hgt - 6, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, `${mins}m (n=${p.n ?? 0})`));
    svg.append(hs('text', { x: x + bw / 2, y: mid - barH - 14, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, `${Math.round(size)}s`));
  });
  return svg;
}

/* ---- All-stops overview table (GET /api/stats/overview) ---- */

// Relative time from a Unix timestamp, e.g. "2 h ago" — distinct from fmtAgo() above,
// which formats an already-elapsed duration rather than a point in time.
function fmtRelativeTs(ts) {
  if (ts == null) return '--';
  const sec = Math.max(0, Math.floor(Date.now() / 1000) - ts);
  if (sec < 60) return `${Math.round(sec)} s ago`;
  if (sec < 3600) return `${Math.round(sec / 60)} m ago`;
  if (sec < 86400) return `${Math.round(sec / 3600)} h ago`;
  return `${Math.round(sec / 86400)} d ago`;
}
// Same ≥80/60-80/<60 thresholds as the on-time badge tones elsewhere in the UI.
function pctTone(pct) {
  if (pct >= 80) return 'good';
  if (pct >= 60) return 'warn';
  return 'bad';
}
function stopLabelFor(stopsCfg, key) {
  const s = stopsCfg.find((st) => st.key === key);
  return (s && s.label) || key;
}

function buildOverviewTable(data, stopsCfg, selectedStop, onSelect) {
  const stops = (data && data.stops) || [];
  if (!stops.length) {
    return h('div', { class: 'card' }, h('h3', {}, 'All stops'), h('p', { class: 'muted small' }, 'No arrivals logged yet.'));
  }
  const rows = stops.map((s) => {
    const pct = s.on_time_pct;
    const known = lateKnownCount(s);
    const selectThis = () => onSelect(s.stop);
    return h('tr', {
      class: `overview-row${s.stop === selectedStop ? ' selected' : ''}`,
      tabindex: '0', role: 'button', 'aria-label': `Show details for ${stopLabelFor(stopsCfg, s.stop)}`,
      onclick: selectThis,
      onkeydown: (ev) => { if (ev.key === 'Enter' || ev.key === ' ') { ev.preventDefault(); selectThis(); } },
    },
      h('td', {}, stopLabelFor(stopsCfg, s.stop)),
      h('td', { class: 'num' }, fmtCount(arrivalCount(s))),
      // A missing percentage is "no data", never a red 0% — see fmtPct.
      h('td', { class: 'num' }, pct == null
        ? h('span', { class: 'muted small' }, 'no data')
        : [h('span', { class: `pct-chip ${pctTone(pct)}` }, fmtPct(pct)),
           known == null ? null : h('span', { class: 'muted small' }, ` of ${known}`)]),
      h('td', { class: 'num' }, fmtMinutes(s.mean_late_min)),
      h('td', { class: 'num' }, fmtCount(s.ghost)),
      h('td', { class: 'num' }, fmtCount(s.noshow)),
      h('td', { class: 'num' }, s.outage_min == null ? 'no data' : `${s.outage_min} min`),
      // Coverage qualifies every other number in the row, so it belongs in the row.
      h('td', { class: 'num' }, s.coverage == null ? 'no data' : `${(Number(s.coverage) <= 1 ? Number(s.coverage) * 100 : Number(s.coverage)).toFixed(0)}%`),
      h('td', {}, fmtRelativeTs(s.last_seen_ts)));
  });
  const table = h('table', { class: 'overview-table' },
    h('thead', {}, h('tr', {},
      h('th', {}, 'Stop'), h('th', { class: 'num' }, 'Arrivals'), h('th', { class: 'num' }, 'On time'),
      h('th', { class: 'num' }, 'Mean late'), h('th', { class: 'num' }, 'Ghosts'), h('th', { class: 'num' }, 'No-shows'),
      h('th', { class: 'num' }, 'Outage'), h('th', { class: 'num' }, 'Coverage'),
      h('th', {}, 'Last seen'))),
    h('tbody', {}, ...rows));
  const card = h('div', { class: 'card' }, h('h3', {}, 'All stops'), h('div', { class: 'table-wrap' }, table),
    h('p', { class: 'small muted' }, 'Click a row to see that stop’s detail charts below.'));
  // Window totals across every stop, with the same "inferred, not measured" caveat.
  const totalSamples = arrivalCount(data);
  if (totalSamples != null) {
    const inf = data.inferred == null ? '' : ` ${data.inferred} of them inferred from a bus leaving the live feed.`;
    card.append(hint(`${totalSamples} arrival${totalSamples === 1 ? '' : 's'} across all stops in this window.${inf}`));
  }
  // The log keeps rows for stops that are no longer configured; the aggregator has fixed
  // room for 8 stops and 3 stations and drops the rest rather than evicting a live one.
  // Say so, so a missing row does not read as missing data.
  const exStops = Number(data.excluded_stops) || 0;
  const exBikes = Number(data.excluded_bikes) || 0;
  if (exStops > 0) {
    card.append(h('p', { class: 'small muted' },
      `${exStops} older stop${exStops === 1 ? '' : 's'} not shown — ${exStops === 1 ? 'it appears' : 'they appear'} in the logs but ${exStops === 1 ? 'is' : 'are'} no longer on the device.`));
  }
  if (exBikes > 0) {
    card.append(h('p', { class: 'small muted' },
      `${exBikes} older Indego station${exBikes === 1 ? '' : 's'} not shown, for the same reason.`));
  }
  const cov = coverageSentence(data.coverage);
  if (cov) card.append(hint(cov));
  return card;
}

/* ---- Crowding charts (data.crowding.by_hour / by_weekday, 0-5 levels) ---- */

const CROWD_LEVEL_NAMES = ['empty', 'open', 'few seats', 'standing', 'packed', 'full'];
function crowdLevelColor(mean) {
  if (mean <= 1) return 'var(--chip-normal)';
  if (mean <= 3) return 'var(--chip-gapped)';
  return 'var(--chip-bunched)';
}
function crowdDistTitle(s) {
  const dist = s.dist || [];
  const parts = CROWD_LEVEL_NAMES.map((name, i) => `${name} ${dist[i] || 0}`);
  return `n=${s.n} · mean level ${s.mean.toFixed(1)} · ${parts.join(', ')}`;
}
// Fills 24 (h) or 7 (wd) fixed-size bins from a sparse array, same idea as
// fillHours/fillWeekdays above but for the {n, mean, dist[6]} crowding shape.
function fillCrowdSlots(arr, count, keyName) {
  const slots = Array.from({ length: count }, (_, i) => ({ [keyName]: i, n: 0, mean: 0, dist: [0, 0, 0, 0, 0, 0] }));
  for (const e of arr || []) {
    const idx = e[keyName] ?? e.h ?? e.wd;
    if (idx >= 0 && idx < count) {
      slots[idx] = { [keyName]: idx, n: e.n || 0, mean: e.mean || 0, dist: Array.isArray(e.dist) && e.dist.length === 6 ? e.dist : [0, 0, 0, 0, 0, 0] };
    }
  }
  return slots;
}
function buildCrowdChart(slots, labelFor) {
  const w = Math.max(320, slots.length * (slots.length > 12 ? 26 : 40));
  const hgt = 130, padB = 20, padT = 8;
  const chartH = hgt - padT - padB;
  const maxV = 5; // fixed 0-5 crowding scale
  const barW = w / slots.length;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Crowding chart' });
  slots.forEach((s, i) => {
    const x = i * barW + 1;
    const bw = Math.max(2, barW - 2);
    if (s.n > 0) {
      const barH = (s.mean / maxV) * chartH;
      const y = padT + chartH - barH;
      const rect = hs('rect', { x, y, width: bw, height: Math.max(1, barH), fill: crowdLevelColor(s.mean) });
      rect.append(hs('title', {}, crowdDistTitle(s)));
      svg.append(rect);
    }
    const lbl = labelFor(s, i);
    if (lbl !== '') svg.append(hs('text', { x: x + bw / 2, y: hgt - 4, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, String(lbl)));
  });
  return svg;
}
function crowdLegend() {
  return h('div', { class: 'chart-legend' },
    h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-normal)' }), '≤1 empty/open'),
    h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-gapped)' }), '2–3 few seats/standing'),
    h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-bunched)' }), '≥4 packed/full'),
    h('span', {}, 'Levels: 0 empty, 1 open, 2 few seats, 3 standing, 4 packed, 5 full. Hover a bar for the full breakdown.'));
}

/* ---- Expected wait / reliability charts (data.wait_by_hour) ---- */

function fillWaitHours(arr) {
  const slots = Array.from({ length: 24 }, (_, hh) => ({ h: hh, n: 0, mean_gap_s: 0, max_gap_s: 0, ghost: 0, noshow: 0 }));
  for (const e of arr || []) {
    if (e.h >= 0 && e.h < 24) slots[e.h] = { h: e.h, n: e.n || 0, mean_gap_s: e.mean_gap_s || 0, max_gap_s: e.max_gap_s || 0, ghost: e.ghost || 0, noshow: e.noshow || 0 };
  }
  return slots;
}
function buildWaitChart(slots) {
  const w = Math.max(320, slots.length * 26);
  const hgt = 150, padL = 26, padB = 20, padT = 10;
  const chartH = hgt - padT - padB;
  const maxV = Math.max(1, ...slots.map((s) => Math.max(s.mean_gap_s, s.max_gap_s) / 60));
  const barW = (w - padL) / slots.length;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Expected wait by hour' });
  const step = Math.ceil(maxV / 4) || 1;
  for (let gv = 0; gv <= maxV; gv += step) {
    const y = padT + chartH - (gv / maxV) * chartH;
    svg.append(hs('line', { x1: padL, x2: w, y1: y, y2: y, stroke: 'var(--border)', 'stroke-width': 1 }));
    svg.append(hs('text', { x: 1, y: y + 3, 'font-size': 9, fill: 'var(--text-muted)' }, String(gv)));
  }
  slots.forEach((s, i) => {
    const x = padL + i * barW + 1;
    const bw = Math.max(2, barW - 2);
    if (s.n > 0) {
      const meanMin = s.mean_gap_s / 60;
      const maxMin = s.max_gap_s / 60;
      const barH = (meanMin / maxV) * chartH;
      const y = padT + chartH - barH;
      svg.append(hs('rect', { x, y, width: bw, height: Math.max(1, barH), fill: 'var(--accent)' }));
      const cx = x + bw / 2;
      const yMax = padT + chartH - (maxMin / maxV) * chartH;
      svg.append(hs('line', { x1: cx, x2: cx, y1: y, y2: yMax, stroke: 'var(--text)', 'stroke-width': 1.5 }));
      svg.append(hs('line', { x1: cx - 3, x2: cx + 3, y1: yMax, y2: yMax, stroke: 'var(--text)', 'stroke-width': 1.5 }));
    }
    if (i % 3 === 0) svg.append(hs('text', { x: x + bw / 2, y: hgt - 4, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, String(s.h)));
  });
  return svg;
}
function buildReliabilityChart(slots) {
  const w = Math.max(320, slots.length * 26);
  const hgt = 140, padL = 22, padB = 20, padT = 8;
  const chartH = hgt - padT - padB;
  const maxV = Math.max(1, ...slots.map((s) => (s.ghost || 0) + (s.noshow || 0)));
  const barW = (w - padL) / slots.length;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Reliability by hour' });
  const step = Math.ceil(maxV / 4) || 1;
  for (let gv = 0; gv <= maxV; gv += step) {
    const y = padT + chartH - (gv / maxV) * chartH;
    svg.append(hs('line', { x1: padL, x2: w, y1: y, y2: y, stroke: 'var(--border)', 'stroke-width': 1 }));
    svg.append(hs('text', { x: 1, y: y + 3, 'font-size': 9, fill: 'var(--text-muted)' }, String(gv)));
  }
  slots.forEach((s, i) => {
    const x = padL + i * barW + 1;
    const bw = Math.max(2, barW - 2);
    const total = (s.ghost || 0) + (s.noshow || 0);
    if (total > 0) {
      const ghostH = ((s.ghost || 0) / maxV) * chartH;
      const yGhost = padT + chartH - ghostH;
      svg.append(hs('rect', { x, y: yGhost, width: bw, height: Math.max(0, ghostH), fill: 'var(--chip-bunched)' }));
      const noshowH = ((s.noshow || 0) / maxV) * chartH;
      const yNoshow = yGhost - noshowH;
      svg.append(hs('rect', { x, y: yNoshow, width: bw, height: Math.max(0, noshowH), fill: 'var(--chip-gapped)' }));
    }
    if (i % 3 === 0) svg.append(hs('text', { x: x + bw / 2, y: hgt - 4, 'font-size': 9, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, String(s.h)));
  });
  return svg;
}

/* ---- Indego availability by hour (overview.bikes, from GET /api/stats/overview) ---- */

function buildBikeHourChart(byHour) {
  const slots = Array.from({ length: 24 }, (_, hh) => ({ h: hh, n: 0, bikes: 0, ebikes: 0, docks: 0 }));
  for (const e of byHour || []) {
    if (e.h >= 0 && e.h < 24) slots[e.h] = { h: e.h, n: e.n || 0, bikes: e.bikes || 0, ebikes: e.ebikes || 0, docks: e.docks || 0 };
  }
  const w = Math.max(320, 24 * 22), hgt = 130, padL = 24, padB = 18, padT = 8;
  const chartH = hgt - padT - padB;
  const maxV = Math.max(1, ...slots.map((s) => Math.max(s.bikes, s.docks)));
  const barW = (w - padL) / 24;
  const svg = hs('svg', { viewBox: `0 0 ${w} ${hgt}`, width: w, height: hgt, role: 'img', 'aria-label': 'Indego availability by hour' });
  const step = Math.ceil(maxV / 4) || 1;
  for (let gv = 0; gv <= maxV; gv += step) {
    const y = padT + chartH - (gv / maxV) * chartH;
    svg.append(hs('line', { x1: padL, x2: w, y1: y, y2: y, stroke: 'var(--border)', 'stroke-width': 1 }));
    svg.append(hs('text', { x: 1, y: y + 3, 'font-size': 8, fill: 'var(--text-muted)' }, String(gv)));
  }
  const dockPts = [];
  slots.forEach((s, i) => {
    const x = padL + i * barW + 1;
    const bw = Math.max(1, barW - 2);
    if (s.n > 0) {
      const classic = Math.max(0, s.bikes - s.ebikes);
      const ebikeH = (s.ebikes / maxV) * chartH;
      const classicH = (classic / maxV) * chartH;
      const yEbike = padT + chartH - ebikeH;
      svg.append(hs('rect', { x, y: yEbike, width: bw, height: Math.max(0, ebikeH), fill: 'var(--chip-gapped)' }));
      const yClassic = yEbike - classicH;
      svg.append(hs('rect', { x, y: yClassic, width: bw, height: Math.max(0, classicH), fill: 'var(--accent)' }));
      const yDock = padT + chartH - (s.docks / maxV) * chartH;
      dockPts.push([x + bw / 2, yDock]);
    }
    if (i % 3 === 0) svg.append(hs('text', { x: x + bw / 2, y: hgt - 4, 'font-size': 8, fill: 'var(--text-muted)', 'text-anchor': 'middle' }, String(s.h)));
  });
  if (dockPts.length > 1) svg.append(hs('polyline', { points: dockPts.map(([px, py]) => `${px},${py}`).join(' '), fill: 'none', stroke: 'var(--text)', 'stroke-width': 1.5 }));
  for (const [px, py] of dockPts) svg.append(hs('circle', { cx: px, cy: py, r: 2, fill: 'var(--text)' }));
  return svg;
}

function buildBikeOverviewCards(bikes) {
  if (!bikes || !bikes.length) return null;
  const wrap = h('div', { class: 'stack' });
  for (const st of bikes) {
    wrap.append(h('div', { class: 'card' }, h('h3', {}, `Indego availability by hour — ${st.name || st.station}`),
      chartWrap(buildBikeHourChart(st.by_hour || [])),
      h('div', { class: 'chart-legend' },
        h('span', {}, h('span', { class: 'swatch', style: 'background:var(--accent)' }), 'Classic bikes (mean)'),
        h('span', {}, h('span', { class: 'swatch', style: 'background:var(--chip-gapped)' }), 'E-bikes (mean)'),
        h('span', {}, h('span', { class: 'swatch', style: 'background:var(--text)' }), 'Docks (mean)'))));
  }
  return wrap;
}

/* ======================== Settings view ======================== */

/* ---- Page structure.
   The settings are grouped into blocks of things that affect each other: Screen,
   Schedules, Alerts, Data, Device, PIN, Firmware. A setting that only matters while
   another one is on sits in a `deps()` box under it, dimmed and disabled while the parent
   is off, so the relationship is visible instead of something to discover by trial. A
   sticky strip at the top jumps between blocks and filters the fields by their label and
   hint text; a sticky bar at the bottom carries the one Save button and says whether
   there is anything to save. Everything the firmware validates (DESIGN.md §6.1) is sent
   exactly as before — this is a re-arrangement of the page, not of the config. */

// One label + control + hint, wrapped so the filter can show or hide it as a unit.
function field(...parts) { return h('div', { class: 'field' }, ...parts); }

// A titled sub-group inside a block; consecutive groups get a divider from the CSS.
function group(...parts) { return h('div', { class: 'group' }, ...parts); }

// Settings that only matter while a parent setting is on. setDeps() dims and disables
// the whole box. The values inside are still collected on save, so switching the parent
// off and on again loses nothing.
function deps(...children) { return h('div', { class: 'dependents' }, ...children); }
function setDeps(box, on) {
  box.classList.toggle('is-off', !on);
  for (const el of box.querySelectorAll('input, select, button')) el.disabled = !on;
}

// A block: a card with a heading, a one-line intro, and the fields. `short` is the label
// on its pill in the sticky nav.
function block(id, short, title, intro, ...children) {
  return h('section', { class: 'card settings-block', id, 'data-short': short },
    h('h2', {}, title), intro ? h('p', { class: 'block-intro' }, intro) : null, ...children);
}

/* ---- Web PIN block.
   The PIN the device asks for before any change. Changing it needs the current one, which
   the X-Pin header on POST /api/pin supplies (and the 401 flow collects if this browser
   has not been told it yet). "Forget" only clears this browser's copy — it does not
   disable the PIN on the device, and the block says so. It is not part of the config
   form: it talks to its own endpoint, so it keeps its own button. */

// Firmware rule: 4-32 printable ASCII, no spaces. Checked here too so a typo is caught
// before it costs a round trip, but the device remains the authority.
const PIN_RE = /^[\x21-\x7e]{4,32}$/;

function buildPinBlock(state) {
  const auth = state.auth || {};
  const msg = h('div', {});

  const newInput = h('input', { type: 'password', id: 'pin-new', autocomplete: 'new-password' });
  const confirmInput = h('input', { type: 'password', id: 'pin-confirm', autocomplete: 'new-password' });
  const saveBtn = h('button', { class: 'primary', onclick: async (ev) => {
    clear(msg);
    const next = newInput.value;
    if (next !== confirmInput.value) {
      msg.append(h('div', { class: 'field-error' }, 'The two PINs do not match.'));
      return;
    }
    if (!PIN_RE.test(next)) {
      msg.append(h('div', { class: 'field-error' },
        'A PIN is 4 to 32 characters, with no spaces. Letters, digits and punctuation are all fine.'));
      return;
    }
    ev.target.disabled = true;
    try {
      await api.setPin(next);
      // The device is now expecting the new PIN, so this browser must remember that one.
      pinStore.set(next);
      newInput.value = '';
      confirmInput.value = '';
      refreshForgetState();
      msg.append(h('div', { class: 'banner ok', role: 'status' }, 'PIN changed, and remembered on this browser.'));
    } catch (e) {
      msg.append(h('div', { class: 'banner danger', role: 'alert' }, e.message));
    } finally {
      ev.target.disabled = false;
    }
  } }, 'Change PIN');

  const forgetState = h('p', { class: 'small muted' }, '');
  function refreshForgetState() {
    forgetState.textContent = pinStore.has()
      ? 'The PIN is saved on this browser.'
      : 'No PIN is saved on this browser.';
  }
  refreshForgetState();

  return block('s-pin', 'PIN', 'Web PIN',
    'The PIN protects everything that changes this device: saving settings, rebooting, '
      + 'resetting Wi-Fi, installing firmware and downloading the logs. Viewing arrivals and '
      + 'statistics never asks for it.',
    auth.pin_required === false
      ? h('p', { class: 'small muted' }, 'This device is not asking for a PIN at the moment.') : null,
    field(h('label', { for: 'pin-new' }, 'New PIN'), newInput,
      hint('4 to 32 characters, no spaces. Write it down somewhere — the device also shows it '
        + 'on its own Device page (tap the screen twice to get there).')),
    field(h('label', { for: 'pin-confirm' }, 'Confirm new PIN'), confirmInput,
      h('div', { class: 'row', style: 'margin-top:.6rem' }, saveBtn), msg),
    group(field(h('h3', {}, 'This browser'), forgetState,
      hint('Forgetting the PIN here does not turn it off on the device — it only clears the '
        + 'saved copy in this browser, so you will be asked for it again the next time you '
        + 'change something. Do this on a shared or borrowed computer.'),
      h('div', { class: 'row' }, h('button', {
        onclick: () => {
          pinStore.clear();
          clear(msg);
          refreshForgetState();
          msg.append(h('div', { class: 'banner ok', role: 'status' }, 'Forgotten. You will be asked for the PIN next time.'));
        },
      }, 'Forget PIN on this browser')))));
}

/* ---- Firmware & maintenance block.
   OTA upload, then the two actions that take the device off the network for a while,
   boxed off in their own danger area so they are never mistaken for ordinary settings. */

function buildFirmwareBlock(state) {
  return block('s-firmware', 'Firmware', 'Firmware & maintenance',
    'Update the software on the device, restart it, or make it forget its Wi-Fi. None of these need Save.',
    field(h('p', {}, 'Version: ', h('strong', {}, state.firmware_version || 'unknown')),
      h('p', {}, 'Board: ', h('strong', {}, state.board || 'unknown')),
      hint('The board name matters when you update: a firmware image built for a different '
        + 'board is refused, and the device says so rather than bricking itself.')),
    field(h('label', { for: 'ota-file' }, 'Upload new firmware (.bin)'),
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
      } }, 'Upload')),
    h('div', { class: 'danger-zone' },
      h('h3', {}, 'Restart and reset'),
      field(h('div', { class: 'row' }, h('button', { class: 'danger', onclick: async () => {
        if (!confirm('Reboot the device now?')) return;
        try { await api.reboot(); alert('Reboot requested.'); } catch (e) { alert(e.message); }
      } }, 'Reboot device')),
      hint('Restarts the device. It is back on the network within about half a minute, and '
        + 'nothing you have saved is lost.')),
      field(h('div', { class: 'row' }, h('button', { class: 'danger', onclick: async () => {
        if (!confirm('Reset Wi-Fi credentials? The device will start its setup access point again.')) return;
        try { await api.wifiReset(); alert('Wi-Fi reset requested.'); } catch (e) { alert(e.message); }
      } }, 'Reset Wi-Fi')),
      hint('Forgets the saved Wi-Fi network and restarts into the setup hotspot, so you join '
        + 'the device from a phone and enter the Wi-Fi password again, as on the first day. '
        + 'Your stops and settings stay as they are.'))));
}

/* ---- The config form.
   Builds every control from `cfg`, arranges them into blocks, and returns the blocks plus
   a collect() that reads the controls back into a whole config object — the same object
   PUT /api/config has always received. */

function buildSettingsForm(cfg, state) {
  const d = cfg.device;

  // ---- Device & network ----
  const nameInput = h('input', { type: 'text', id: 'set-name', value: d.name });
  const mdnsPreview = h('span', { class: 'muted small' }, `${slugify(d.name)}.local`);
  nameInput.addEventListener('input', () => { mdnsPreview.textContent = `${slugify(nameInput.value)}.local`; });

  const tzSelect = h('select', { id: 'set-tz' },
    ...US_TIMEZONES.map(([label, tz]) => h('option', { value: tz, selected: tz === d.tz || undefined }, label)),
    h('option', { value: '__custom__', selected: !US_TIMEZONES.some(([, tz]) => tz === d.tz) || undefined }, 'Custom…'));
  const tzCustom = h('input', { type: 'text', id: 'set-tz-custom', value: d.tz, placeholder: 'POSIX TZ string' });
  tzCustom.classList.toggle('hidden', tzSelect.value !== '__custom__');
  tzSelect.addEventListener('change', () => tzCustom.classList.toggle('hidden', tzSelect.value !== '__custom__'));

  // ---- Data ----
  const pollInput = h('input', { type: 'number', id: 'set-poll', min: 15, max: 120, value: d.poll_seconds });
  const loggingInput = h('input', { type: 'checkbox', id: 'set-logging' });
  loggingInput.checked = d.logging;
  // Data connection (DESIGN.md §2.1): only offered when this firmware reports a `transport`
  // block in /api/state, i.e. was built with HTTPS. A build without it fetches over plain HTTP
  // whatever the config says, so showing the choice there would be a lie.
  const hasTransport = !!(state && state.transport);
  const TRANSPORTS = [
    ['http', 'Plain HTTP (default)'],
    ['https_preferred', 'Encrypted when memory allows'],
    ['https', 'Encrypted only - show "unavailable" rather than use plain HTTP'],
  ];
  const transportSelect = h('select', { id: 'set-transport' },
    ...TRANSPORTS.map(([v, label]) => h('option', { value: v, selected: v === (d.transport || 'http') || undefined }, label)));

  // ---- Screen ----
  const ROTATIONS = [[0, 'Portrait (0°)'], [90, 'Landscape (90°)'], [180, 'Portrait, flipped (180°)'], [270, 'Landscape, flipped (270°)']];
  const rotSelect = h('select', { id: 'set-rotation' },
    ...ROTATIONS.map(([deg, label]) => h('option', { value: String(deg), selected: deg === (d.rotation ?? 0) || undefined }, label)));
  const brightInput = h('input', { type: 'range', id: 'set-bright', min: 10, max: 100, value: d.brightness });
  const brightVal = h('span', { class: 'small muted' }, `${d.brightness}%`);
  brightInput.addEventListener('input', () => { brightVal.textContent = `${brightInput.value}%`; });
  const THEMES = [['light', 'Light'], ['dark', 'Dark']];
  const themeSelect = h('select', { id: 'set-theme' },
    ...THEMES.map(([v, label]) => h('option', { value: v, selected: v === (d.theme || 'light') || undefined }, label)));
  const invertInput = h('input', { type: 'checkbox', id: 'set-invert' });
  invertInput.checked = !!d.invert_colors;
  const largeTextInput = h('input', { type: 'checkbox', id: 'set-large-text' });
  largeTextInput.checked = !!d.large_text;
  const crowdingMode = d.crowding || (d.show_crowding === false ? 'off' : 'words');
  const CROWDING_MODES = [['off', 'Off'], ['words', 'Words'], ['icons', 'Icons'], ['both', 'Icons + word']];
  const crowdingSelect = h('select', { id: 'set-crowding' },
    ...CROWDING_MODES.map(([v, label]) => h('option', { value: v, selected: v === crowdingMode || undefined }, label)));
  const CROWDING_ICON_SCHEMES = [['seats', 'Seats then people'], ['crowd', 'Crowd meter (people only)']];
  const crowdingIconsSelect = h('select', { id: 'set-crowding-icons' },
    ...CROWDING_ICON_SCHEMES.map(([v, label]) => h('option', { value: v, selected: v === (d.crowding_icons || 'seats') || undefined }, label)));
  // The icon scheme only matters while icons are shown at all.
  const crowdIconDeps = deps(
    field(h('label', { for: 'set-crowding-icons' }, 'Crowding icons'), crowdingIconsSelect,
      hint('“Seats then people” is three chairs that empty out as the seats fill: 3 green chairs for empty, 2 green for open, 1 amber chair for few seats, then 1 amber person for standing, 2 red people for packed and 3 red people for full. “Crowd meter” is just one to three people: 1 green for empty or open, 2 amber for few seats or standing, 3 red for packed or full. Picking “Icons + word” above shows both.')));
  const refreshCrowd = () => setDeps(crowdIconDeps, crowdingSelect.value === 'icons' || crowdingSelect.value === 'both');
  crowdingSelect.addEventListener('change', refreshCrowd);
  refreshCrowd();

  const hdr = d.header || {};
  const HEADER_ITEMS = [['name', 'Device name', false], ['clock', 'Clock', true], ['weather', 'Current weather', true], ['wifi', 'Wi-Fi signal', true], ['updated', '"updated N s ago"', true]];
  const headerInputs = {};
  const headerRows = HEADER_ITEMS.map(([k, label, dflt]) => {
    const cb = h('input', { type: 'checkbox', id: `set-hdr-${k}` });
    cb.checked = hdr[k] ?? dflt;
    headerInputs[k] = cb;
    return h('label', { class: 'inline' }, cb, ` ${label}`);
  });

  // ---- Alerts: fetch switch -> ticker contents -> ticker size/speed ----
  const alertsInput = h('input', { type: 'checkbox', id: 'set-alerts' });
  alertsInput.checked = cfg.alerts;
  const tickerLines = d.ticker_lines ?? 3;
  const tickerLinesSelect = h('select', { id: 'set-ticker-lines' },
    ...[1, 2, 3, 4, 5, 6, 7, 8].map((n) => h('option', { value: String(n), selected: n === tickerLines || undefined },
      n === 1 ? '1 line (scrolls sideways)' : `${n} lines (scrolls upward)`)));
  const tickerSpeed = d.ticker_speed ?? 30;
  const tickerSpeedInput = h('input', { type: 'range', id: 'set-ticker-speed', min: 5, max: 120, value: tickerSpeed });
  const tickerSpeedVal = h('span', { class: 'small muted' }, `${tickerSpeed} px/s`);
  tickerSpeedInput.addEventListener('input', () => { tickerSpeedVal.textContent = `${tickerSpeedInput.value} px/s`; });
  const TICKER_SHOW = [['both', 'Alerts and detours'], ['alerts', 'Alerts only'], ['detours', 'Detours only'], ['off', 'Nothing (ticker off)']];
  const tickerShow = d.ticker_show || 'both';
  const tickerShowSelect = h('select', { id: 'set-ticker-show' },
    ...TICKER_SHOW.map(([v, label]) => h('option', { value: v, selected: v === tickerShow || undefined }, label)));
  const tickerSizeDeps = deps(
    field(h('label', { for: 'set-ticker-lines' }, 'Height'), tickerLinesSelect,
      hint('One line scrolls sideways like a news ticker. Two or more lines wrap the text and scroll it upward instead, which is easier to read but takes room from the arrivals.')),
    field(h('label', { for: 'set-ticker-speed' }, 'Scroll speed'), h('div', { class: 'row' }, tickerSpeedInput, tickerSpeedVal),
      hint('How fast the text moves, in pixels a second. Slower is easier to read from across the room.')));
  const tickerDeps = deps(
    field(hint('Service alerts and detours for your routes scroll along the bottom of the screen.')),
    field(h('label', { for: 'set-ticker-show' }, 'Show'), tickerShowSelect,
      hint('Which messages the ticker carries. The “Show service alerts” switch above decides whether they are fetched from SEPTA at all.')),
    tickerSizeDeps);
  // Outer box first, then the inner one: enabling the outer re-enables everything in it.
  const refreshTicker = () => {
    setDeps(tickerDeps, alertsInput.checked);
    setDeps(tickerSizeDeps, alertsInput.checked && tickerShowSelect.value !== 'off');
  };
  alertsInput.addEventListener('change', refreshTicker);
  tickerShowSelect.addEventListener('change', refreshTicker);
  refreshTicker();

  // ---- Time to leave (top-level due) ----
  const due = cfg.due || {};
  const dueEnabled = h('input', { type: 'checkbox', id: 'set-due-enabled' });
  dueEnabled.checked = due.enabled ?? true;
  const dueMinutes = h('input', { type: 'number', id: 'set-due-minutes', min: 1, max: 15, value: due.minutes ?? 3 });
  const dueLed = h('input', { type: 'checkbox', id: 'set-due-led' });
  dueLed.checked = due.led ?? true;
  const dueScreen = h('input', { type: 'checkbox', id: 'set-due-screen' });
  dueScreen.checked = due.screen ?? true;
  const dueChime = h('input', { type: 'checkbox', id: 'set-due-chime' });
  dueChime.checked = !!due.chime;
  const dueDeps = deps(
    field(h('label', { for: 'set-due-minutes' }, 'Minutes before arrival'), dueMinutes,
      hint('Roughly how long it takes you to walk to the stop.')),
    field(h('label', { class: 'inline' }, dueLed, ' Blink the LED'),
      hint('Blinks the small coloured LED on the board green — easy to catch from the corner of your eye.')),
    field(h('label', { class: 'inline' }, dueScreen, ' Blink the row on screen'),
      hint('Flashes that arrival’s minutes on the display.')),
    field(h('label', { class: 'inline' }, dueChime, ' Two short beeps (board speaker)'),
      hint('Two short beeps, once per bus. Needs a board with a speaker fitted, and stays silent during quiet hours.')));
  dueEnabled.addEventListener('change', () => setDeps(dueDeps, dueEnabled.checked));
  setDeps(dueDeps, dueEnabled.checked);

  // ---- Quiet hours (device.quiet) ----
  const quiet = d.quiet || {};
  const quietEnabled = h('input', { type: 'checkbox', id: 'set-quiet-enabled' });
  quietEnabled.checked = !!quiet.enabled;
  const quietStart = h('input', { type: 'time', id: 'set-quiet-start', value: quiet.start || '23:00' });
  const quietEnd = h('input', { type: 'time', id: 'set-quiet-end', value: quiet.end || '06:00' });
  const quietBrightness = h('input', { type: 'range', id: 'set-quiet-bright', min: 0, max: 50, value: quiet.brightness ?? 0 });
  const quietBrightVal = h('span', { class: 'small muted' }, `${quiet.brightness ?? 0}% (0 = screen off)`);
  quietBrightness.addEventListener('input', () => { quietBrightVal.textContent = `${quietBrightness.value}% (0 = screen off)`; });
  const quietWake = h('input', { type: 'number', id: 'set-quiet-wake', min: 5, max: 300, value: quiet.wake_seconds ?? 30 });
  const quietDeps = deps(
    field(h('div', { class: 'row' },
      h('div', {}, h('label', { for: 'set-quiet-start' }, 'Start'), quietStart),
      h('div', {}, h('label', { for: 'set-quiet-end' }, 'End'), quietEnd)),
      hint('Quiet hours run from the first time to the second, and may cross midnight.')),
    field(h('label', { for: 'set-quiet-bright' }, 'Brightness during quiet hours'), h('div', { class: 'row' }, quietBrightness, quietBrightVal),
      hint('How dim the screen goes. Zero turns the backlight off completely.')),
    field(h('label', { for: 'set-quiet-wake' }, 'Wake for N seconds on touch'), quietWake,
      hint('A tap brings the screen back to normal brightness for this long, then it dims again. It stays on the page you were looking at.')));
  quietEnabled.addEventListener('change', () => setDeps(quietDeps, quietEnabled.checked));
  setDeps(quietDeps, quietEnabled.checked);

  // ---- Night clock (device.night) ----
  const night = d.night || {};
  const nightEnabled = h('input', { type: 'checkbox', id: 'set-night-enabled' });
  nightEnabled.checked = night.enabled ?? true;
  const nightAfter = h('input', { type: 'number', id: 'set-night-after', min: 15, max: 240, value: night.after_min ?? 60 });
  const nightDeps = deps(
    field(h('label', { for: 'set-night-after' }, 'Show the clock when nothing is due within N minutes'), nightAfter,
      hint('How quiet it has to get first. A lower number switches to the clock sooner.')));
  nightEnabled.addEventListener('change', () => setDeps(nightDeps, nightEnabled.checked));
  setDeps(nightDeps, nightEnabled.checked);

  // ---- Weather ----
  const wx = cfg.weather || {};
  const wxEnabled = h('input', { type: 'checkbox', id: 'set-wx' });
  wxEnabled.checked = wx.enabled ?? true;
  const wxPerStop = h('input', { type: 'checkbox', id: 'set-wx-stop' });
  wxPerStop.checked = wx.per_stop ?? true;
  const wxUnits = h('select', { id: 'set-wx-units' },
    h('option', { value: 'f', selected: (wx.units || 'f') === 'f' || undefined }, '°F'),
    h('option', { value: 'c', selected: wx.units === 'c' || undefined }, '°C'));
  const wxDeps = deps(
    field(h('label', { class: 'inline' }, wxPerStop, ' Note the forecast at each stop’s next arrival when it differs (rain, snow, fog)'),
      hint('Adds a short line to a stop’s panel when the weather around its next arrival is worth knowing about — rain, snow or fog. Nothing is shown on ordinary days.')),
    field(h('label', { for: 'set-wx-units' }, 'Units'), wxUnits,
      hint('Fahrenheit or Celsius, for every temperature the device shows.')),
    field(h('p', { class: 'small muted' }, 'Forecasts come from Open-Meteo.com (free, no account) for each stop’s coordinates; stops within about a mile share one forecast. Stops added before this version may need coordinates - see the Stops page.')));
  wxEnabled.addEventListener('change', () => setDeps(wxDeps, wxEnabled.checked));
  setDeps(wxDeps, wxEnabled.checked);

  // ---- Profiles (top-level profiles, max 4) ----
  const DAY_LABELS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  const profiles = (cfg.profiles || []).map((p) => ({
    name: p.name || '', days: [...(p.days || [])], start: p.start || '05:30', end: p.end || '10:00',
    order: [...(p.stops || []), ...cfg.stops.map((s) => s.key).filter((k) => !(p.stops || []).includes(k))],
    included: new Set(p.stops || []),
  }));
  const profilesCard = h('div', {});

  function profileCard(p, pi) {
    const nameInput2 = h('input', { type: 'text', value: p.name, placeholder: `Profile ${pi + 1}` });
    nameInput2.addEventListener('input', () => { p.name = nameInput2.value; });
    const dayBoxes = DAY_LABELS.map((lbl, di) => {
      const cb = h('input', { type: 'checkbox' });
      cb.checked = p.days.includes(di);
      cb.addEventListener('change', () => {
        p.days = cb.checked ? [...new Set([...p.days, di])] : p.days.filter((x) => x !== di);
      });
      return h('label', { class: 'inline' }, cb, ` ${lbl}`);
    });
    const startInput = h('input', { type: 'time', value: p.start });
    startInput.addEventListener('change', () => { p.start = startInput.value; });
    const endInput = h('input', { type: 'time', value: p.end });
    endInput.addEventListener('change', () => { p.end = endInput.value; });

    const stopListDiv = h('div', {});
    function renderStopList() {
      clear(stopListDiv);
      if (!cfg.stops.length) { stopListDiv.append(h('p', { class: 'small muted' }, 'No stops configured yet.')); return; }
      p.order.forEach((key, oi) => {
        const stop = cfg.stops.find((st) => st.key === key);
        if (!stop) return;
        const cb = h('input', { type: 'checkbox' });
        cb.checked = p.included.has(key);
        cb.addEventListener('change', () => { if (cb.checked) p.included.add(key); else p.included.delete(key); });
        stopListDiv.append(h('div', { class: 'row' },
          h('label', { class: 'inline' }, cb, ` ${stop.label}`),
          h('span', { class: 'spacer' }),
          h('button', { class: 'icon', title: 'Move up', disabled: oi === 0, onclick: () => { [p.order[oi - 1], p.order[oi]] = [p.order[oi], p.order[oi - 1]]; renderStopList(); } }, '↑'),
          h('button', { class: 'icon', title: 'Move down', disabled: oi === p.order.length - 1, onclick: () => { [p.order[oi + 1], p.order[oi]] = [p.order[oi], p.order[oi + 1]]; renderStopList(); } }, '↓')));
      });
    }
    renderStopList();

    return h('div', { class: 'subcard' },
      h('div', { class: 'row between' },
        h('h3', {}, `Profile ${pi + 1}`),
        h('button', { class: 'icon danger', title: 'Remove profile', 'aria-label': 'Remove profile', onclick: () => { profiles.splice(pi, 1); renderProfiles(); } }, '✕')),
      h('label', {}, 'Name'), nameInput2,
      h('label', {}, 'Days'), h('div', { class: 'row' }, ...dayBoxes),
      h('div', { class: 'row' },
        h('div', {}, h('label', {}, 'Start'), startInput),
        h('div', {}, h('label', {}, 'End'), endInput)),
      h('label', {}, 'Stops shown, in order'), stopListDiv);
  }

  function renderProfiles() {
    clear(profilesCard);
    profilesCard.append(h('p', { class: 'small muted' }, 'While a profile is active the screen shows only its stops, in this order. All stops keep polling and logging.'));
    profiles.forEach((p, pi) => profilesCard.append(profileCard(p, pi)));
    profilesCard.append(h('button', {
      disabled: profiles.length >= 4,
      onclick: () => {
        profiles.push({ name: '', days: [], start: '05:30', end: '10:00', order: cfg.stops.map((s) => s.key), included: new Set() });
        renderProfiles();
      },
    }, '+ Add profile'));
    if (profiles.length >= 4) profilesCard.append(h('p', { class: 'small muted' }, 'Maximum of 4 profiles reached.'));
  }
  renderProfiles();

  // ---- Assemble the blocks ----
  const sections = [
    block('s-screen', 'Screen', 'Screen & appearance',
      'How the device’s screen looks: colours, brightness, which way up, how big the text is, and what the top strip shows.',
      field(h('label', { for: 'set-theme' }, 'Screen theme'), themeSelect,
        hint('Light or dark colours on the device’s own screen. This web page follows your browser instead.')),
      field(h('label', { for: 'set-bright' }, 'Screen brightness'), h('div', { class: 'row' }, brightInput, brightVal),
        hint('How bright the backlight is during the day. Quiet hours, under Schedules, can dim it further at night.')),
      field(h('label', { for: 'set-rotation' }, 'Screen rotation'), rotSelect,
        hint('Which way up the display is mounted. 0° is the tall portrait orientation the layout was designed around; 90° turns it on its side.')),
      field(h('label', { class: 'inline' }, invertInput, ' Invert panel colors'),
        hint('Only turn this on if colours look wrong on your panel — the light theme looking dark, the route badge orange instead of blue, or a white flash at boot. Otherwise leave it alone.')),
      field(h('label', { class: 'inline' }, largeTextInput, ' Large text (two rows per stop, big numbers)'),
        hint('Two rows per stop with the minutes in large digits, readable from across a room. Fewer arrivals fit on the screen this way.')),
      group(
        field(h('label', { for: 'set-crowding' }, 'Crowding'), crowdingSelect,
          hint('SEPTA reports how full each bus is, and the display can show it next to the destination as: empty, open (many seats), few seats, standing (standing room only), packed (crushed standing) or full (not boarding). Not every bus reports crowding — when one doesn’t, nothing is shown for it.')),
        crowdIconDeps),
      group(
        field(h('h3', {}, 'Top strip'),
          hint('The strip along the top of the device’s screen is narrow, so pick what it shows. Anything unticked is simply not drawn.'),
          h('div', { class: 'checks' }, ...headerRows)))),

    block('s-schedules', 'Schedules', 'Schedules',
      'What the screen shows when. These three layer on top of each other: profiles pick which stops are shown, the night clock takes over when nothing is due, and quiet hours dim the screen regardless of what is on it.',
      group(
        field(h('label', { class: 'inline toggle' }, quietEnabled, ' Quiet hours'),
          hint('Dims or blanks the screen overnight so it is not glowing at you in bed. A tap on the screen wakes it briefly.')),
        quietDeps),
      group(
        field(h('label', { class: 'inline toggle' }, nightEnabled, ' Night clock'),
          hint('When nothing is due for a while the screen becomes a big clock with the date, the weather and each stop’s next departure — so it is useful the rest of the time too.')),
        nightDeps),
      group(
        field(h('h3', {}, 'Commute profiles'),
          hint('Profiles decide which stops appear at which times of the week — the outbound stop on weekday mornings, the inbound one in the evening. Outside every profile’s days and hours, all your stops are shown. Every stop keeps polling and logging either way.'),
          profilesCard))),

    block('s-alerts', 'Alerts', 'Alerts & reminders',
      'The ways the display gets your attention: SEPTA’s service alerts on the ticker, and a nudge when it is time to leave.',
      group(
        field(h('label', { class: 'inline toggle' }, alertsInput, ' Show service alerts'),
          hint('Fetches SEPTA’s alerts and detours for your routes. The ticker settings below pick which of them are displayed.')),
        tickerDeps),
      group(
        field(h('label', { class: 'inline toggle' }, dueEnabled, ' Time to leave'),
          hint('Nudges you the moment an arrival first comes within the minutes below, so you can leave without watching the screen.')),
        dueDeps)),

    block('s-data', 'Data & weather', 'Data & weather',
      'Where the numbers come from, how often, and what is kept.',
      field(h('label', { for: 'set-poll' }, 'Poll interval (seconds)'), pollInput,
        hint('How often the device asks SEPTA for fresh times. It speeds up to every 15 seconds by itself whenever a bus is less than 3 minutes away, so this setting only affects the quiet stretches.')),
      field(h('label', { class: 'inline' }, loggingInput, ' Log arrivals to SD card'),
        hint('Writes every arrival to the SD card so the Stats page has something to work from. Turn it off and the Stats page stays empty.')),
      hasTransport ? field(h('label', { for: 'set-transport' }, 'Data connection'), transportSelect,
        hint('How the device talks to SEPTA, the weather service and Indego. Encrypted (HTTPS) means nobody on your network can alter the times on the way in. Read this before switching it on: one encrypted connection needs about 50 KB of the device\u2019s memory all at once, and this board has closer to 30 KB free at the moment a fetch starts, so on today\u2019s hardware it never actually fits. \u201cEncrypted when memory allows\u201d asks for HTTPS on every fetch and uses plain HTTP only when memory is short - never because a certificate failed; that fetch simply fails and is retried next time. On this board that means it will keep reporting \u201cPlain HTTP this time\u201d on the Now page. \u201cEncrypted only\u201d never uses plain HTTP: a stop reads unavailable until the device can afford the encrypted connection, which here means always - it is here for the day the memory problem is fixed, not for today. \u201cPlain HTTP (default)\u201d never asks for HTTPS. The Now page\u2019s status strip shows which one the latest arrivals came over.')) : null,
      group(
        field(h('label', { class: 'inline toggle' }, wxEnabled, ' Show weather'),
          hint('Puts the current temperature and a condition icon in the top strip of the screen.')),
        wxDeps)),

    block('s-device', 'Device', 'Device & network',
      'The device’s name on your home network, and the clock it keeps.',
      field(h('label', { for: 'set-name' }, 'Device name'), h('div', { class: 'row' }, nameInput, mdnsPreview),
        hint('What the device calls itself. It is also the web address of this page on your network: name.local in any browser at home.')),
      field(h('label', { for: 'set-tz' }, 'Timezone'), tzSelect, tzCustom,
        hint('Every clock time the device shows is in this zone. Eastern is the right one for Philadelphia, and it changes for daylight saving on its own.'))),

    buildPinBlock(state),
    buildFirmwareBlock(state),
  ];

  function collect() {
    const tz = tzSelect.value === '__custom__' ? tzCustom.value.trim() : tzSelect.value;
    // Drop legacy fields the current UI never sets: show_crowding (folded into
    // crowding on load) and use_https/tls_verify (HTTPS mode removed from firmware —
    // strip them here too so an old config fetched from the device doesn't cause them
    // to be echoed back on save).
    const { show_crowding: _legacyShowCrowding, use_https: _legacyUseHttps, tls_verify: _legacyTlsVerify, ...dRest } = d;
    return {
      ...cfg,
      device: {
        ...dRest, name: nameInput.value.trim(), tz, poll_seconds: Number(pollInput.value),
        brightness: Number(brightInput.value), rotation: Number(rotSelect.value),
        theme: themeSelect.value, invert_colors: invertInput.checked,
        ticker_lines: Number(tickerLinesSelect.value), ticker_speed: Number(tickerSpeedInput.value),
        ticker_show: tickerShowSelect.value,
        logging: loggingInput.checked,
        ...(hasTransport ? { transport: transportSelect.value } : {}),
        header: Object.fromEntries(Object.entries(headerInputs).map(([k, cb]) => [k, cb.checked])),
        large_text: largeTextInput.checked,
        crowding: crowdingSelect.value,
        crowding_icons: crowdingIconsSelect.value,
        quiet: {
          enabled: quietEnabled.checked, start: quietStart.value, end: quietEnd.value,
          brightness: Number(quietBrightness.value), wake_seconds: Number(quietWake.value),
        },
        night: { enabled: nightEnabled.checked, after_min: Number(nightAfter.value) },
      },
      alerts: alertsInput.checked,
      weather: { enabled: wxEnabled.checked, per_stop: wxPerStop.checked, units: wxUnits.value },
      due: {
        enabled: dueEnabled.checked, minutes: Number(dueMinutes.value),
        led: dueLed.checked, screen: dueScreen.checked, chime: dueChime.checked,
      },
      profiles: profiles.map((p, pi) => ({
        name: p.name.trim() || `Profile ${pi + 1}`,
        days: [...p.days].sort((a, b) => a - b),
        start: p.start,
        end: p.end,
        stops: p.order.filter((k) => p.included.has(k)),
      })),
    };
  }

  return { sections, collect };
}

let settingsActive = false;

async function renderSettings(root) {
  settingsActive = true;
  const listeners = [];
  activeCleanup = () => {
    settingsActive = false;
    for (const [ev, fn] of listeners) window.removeEventListener(ev, fn);
  };
  root.append(h('p', { class: 'muted' }, 'Connecting to the display…'));
  let cfg;
  // state is best-effort and loaded separately, below — it only supplies the firmware
  // version/board line and the "restored its previous settings" banner, and it must
  // never be able to hold the form itself hostage. It did, once: /api/state and
  // /api/config are refused independently (DESIGN.md §12.1), so a device that's happy
  // to answer /api/config but still busy on /api/state used to leave this page stuck on
  // "Connecting…" forever even though there was nothing actually wrong with the config.
  let state = {};
  try {
    // A heavy, memory-gated read (DESIGN.md §12.1): retries a refusal on its own and
    // does not reject for that reason, so this only throws on something genuinely
    // unexpected.
    cfg = await api.config();
  } catch (e) { if (!settingsActive) return; clear(root); root.append(h('div', { class: 'banner danger' }, e.message)); return; }
  if (!settingsActive) return;
  clear(root);

  // ---- Sticky nav: block pills + the filter box ----
  const filterInput = h('input', { type: 'search', id: 'settings-filter', placeholder: 'Filter settings…', 'aria-label': 'Filter settings' });
  const pills = h('div', { class: 'pills' });
  const nav = h('div', { class: 'settings-nav' }, filterInput, pills);
  const noMatch = h('p', { class: 'muted hidden', role: 'status' });
  const content = h('div', { class: 'stack' });

  // ---- Sticky save bar ----
  const msg = h('div', { class: 'msg' });
  const status = h('span', { class: 'status' });
  // The .long halves drop out on narrow phones so the bar stays one row.
  const resetBtn = h('button', { title: 'Put every setting back to what the device has saved', onclick: () => paint() }, 'Undo', h('span', { class: 'long' }, ' changes'));
  const saveBtn = h('button', { class: 'primary', onclick: save }, 'Save', h('span', { class: 'long' }, ' settings'));
  const bar = h('div', { class: 'save-bar' }, msg, status, resetBtn, saveBtn);

  root.append(nav);
  // The firmware sets config_recovered when it had to fall back to the last known-good
  // config after a bad save — silence here would let the owner wonder why a setting
  // reverted itself. A placeholder element (not a one-time conditional append) because
  // state can resolve after this page is already up — see the api.state() call at the
  // bottom of this function.
  const recoveredBanner = h('div', {});
  function paintRecoveredBanner() {
    clear(recoveredBanner);
    if (state.config_recovered) {
      recoveredBanner.append(h('div', { class: 'banner warn', role: 'status' },
        'The device restored its previous settings after a bad save. Check the settings below still say what you want, then save again.'));
    }
  }
  root.append(recoveredBanner, noMatch, content, bar);

  let form = null;      // { sections, collect } from buildSettingsForm
  let baseline = '';    // JSON of the form as loaded or last saved; "unsaved" is a string compare
  let stickyH = 0;      // height of the top bar + settings nav, for scroll offsets

  // (Re)build the whole form from cfg: on first load, and for "Undo changes".
  function paint() {
    const y = window.scrollY;
    form = buildSettingsForm(cfg, state);
    clear(content);
    content.append(...form.sections);
    clear(pills);
    for (const s of form.sections) {
      pills.append(h('button', { class: 'pill', type: 'button', 'data-target': s.id, onclick: () => jumpTo(s) }, s.dataset.short));
    }
    baseline = JSON.stringify(form.collect());
    clear(msg);
    applyFilter();
    checkDirty();
    window.scrollTo(0, y);
  }

  function checkDirty() {
    const dirty = !!form && JSON.stringify(form.collect()) !== baseline;
    bar.classList.toggle('dirty', dirty);
    saveBtn.disabled = !dirty;
    resetBtn.disabled = !dirty;
    status.textContent = dirty ? 'Unsaved changes' : 'No unsaved changes';
    // A "saved" banner next to "unsaved changes" would contradict itself.
    if (dirty && msg.querySelector('.ok')) clear(msg);
  }
  // Every control lives under `content`, so three delegated listeners cover them all —
  // the profile buttons (add, remove, reorder) mutate their state before the click
  // bubbles up here.
  content.addEventListener('input', checkDirty);
  content.addEventListener('change', checkDirty);
  content.addEventListener('click', (ev) => { if (ev.target.closest('button')) checkDirty(); });

  async function save() {
    const next = form.collect();
    clear(msg);
    saveBtn.disabled = true;
    resetBtn.disabled = true;
    status.textContent = 'Saving…';
    try {
      await api.saveConfig(next);
      cfg = next;
      baseline = JSON.stringify(form.collect());
      msg.append(h('div', { class: 'banner ok', role: 'status' }, 'Settings saved.'));
    } catch (e) {
      // The typed edits are never touched here — `next` above came from the form, and
      // failure leaves `cfg`/`form` exactly as they were, so nothing the owner typed is
      // lost. They just need to press Save again.
      const { cls, text } = writeErrorMessage(e);
      msg.append(h('div', { class: `banner ${cls}`, role: cls === 'danger' ? 'alert' : 'status' }, text));
    }
    checkDirty();
  }

  // ---- Filter: hide every field whose label/hint/options do not contain the text ----
  function applyFilter() {
    const q = filterInput.value.trim().toLowerCase();
    let any = false;
    form.sections.forEach((s, i) => {
      let shown = 0;
      for (const f of s.querySelectorAll('.field')) {
        const hit = !q || f.textContent.toLowerCase().includes(q);
        f.classList.toggle('hidden', !hit);
        if (hit) shown++;
      }
      // A box with nothing left in it would show as a bare divider or an empty border.
      for (const g of s.querySelectorAll('.group, .dependents, .danger-zone')) {
        g.classList.toggle('hidden', !!q && !g.querySelector('.field:not(.hidden)'));
      }
      s.classList.toggle('hidden', !!q && !shown);
      pills.children[i].classList.toggle('empty', !!q && !shown);
      if (shown) any = true;
    });
    noMatch.textContent = `No settings match “${filterInput.value.trim()}”.`;
    noMatch.classList.toggle('hidden', !q || any);
    updateActive();
  }
  filterInput.addEventListener('input', debounce(applyFilter, 60));

  // ---- Jump links and the "you are here" pill ----
  // An instant jump, not a smooth one: the block's scroll-margin-top keeps it clear of the
  // sticky strips, and a page of settings is not the place for motion.
  function jumpTo(s) {
    s.scrollIntoView({ block: 'start' });
    updateActive();
  }
  function measure() {
    const top = ($('#topbar') || {}).offsetHeight || 0;
    nav.style.top = `${top}px`;
    stickyH = top + nav.offsetHeight;
    root.style.setProperty('--sticky-h', `${stickyH}px`);
  }
  function updateActive() {
    const vis = form.sections.filter((s) => !s.classList.contains('hidden'));
    let cur = vis[0];
    for (const s of vis) if (s.getBoundingClientRect().top <= stickyH + 24) cur = s;
    // At the very bottom the last block is the one on screen even if its top never
    // reaches the line.
    if (window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 2) cur = vis[vis.length - 1];
    for (const p of pills.children) p.classList.toggle('active', !!cur && p.dataset.target === cur.id);
  }
  let ticking = false;
  const onScroll = () => {
    if (ticking) return;
    ticking = true;
    requestAnimationFrame(() => { ticking = false; updateActive(); });
  };
  const onResize = () => { measure(); updateActive(); };
  // Closing the tab with edits pending gets the browser's own "leave page?" prompt.
  const onUnload = (ev) => { if (bar.classList.contains('dirty')) { ev.preventDefault(); ev.returnValue = ''; } };
  listeners.push(['scroll', onScroll], ['resize', onResize], ['beforeunload', onUnload]);
  window.addEventListener('scroll', onScroll, { passive: true });
  window.addEventListener('resize', onResize);
  window.addEventListener('beforeunload', onUnload);

  paint();
  measure();
  updateActive();
  paintRecoveredBanner();

  // state loads in the background, independent of cfg above (see the comment where
  // `state` is declared). If it's still refusing by the time the owner has started
  // typing, leaving the firmware-version line as "unknown" a while longer is a far
  // smaller cost than the alternative: repainting the whole form out from under an
  // edit in progress would silently drop it. So this only ever repaints when the form
  // is clean — checkDirty()/the save bar's "dirty" class is the single source of truth
  // for that, same as Undo changes uses.
  api.state().then((s) => {
    if (!settingsActive) return;
    state = s || {};
    paintRecoveredBanner();
    if (!bar.classList.contains('dirty')) paint();
  }).catch(() => {});
}

#!/usr/bin/env node
// Mock device server for Philly Transit Display web UI development.
// Implements every route in DESIGN.md §7 using Node built-ins only (no npm deps),
// backed by the real SEPTA fixtures in firmware/test/fixtures/ where practical.
//
// Usage: node web/mock-server.mjs [port]   (default 8080)

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const webDir = __dirname;
const fixturesDir = path.join(__dirname, '..', 'firmware', 'test', 'fixtures');

const argPort = process.argv[2];
const PORT = argPort ? Number(String(argPort).replace(/^port=/, '')) || 8080 : 8080;

/* ------------------------------ fixtures ------------------------------ */

function readJSON(file, fallback) {
  try { return JSON.parse(fs.readFileSync(path.join(fixturesDir, file), 'utf8')); }
  catch (e) { return fallback; }
}

const FIX = {
  stops17: readJSON('stops_17.json', []),
  stops34: readJSON('stops_34.json', []),
  stopsBSL: readJSON('stops_BSL.json', []),
  transitview17: readJSON('transitview_17.json', { bus: [] }),
  sched21332: readJSON('busschedules_21332.json', {}),
  sched21297: readJSON('busschedules_21297.json', {}),
  alerts17: readJSON('alerts_bus_17.json', []),
  arrivals30th: readJSON('arrivals_30th.json', {}),
};

const RAIL_STATIONS_BUILTIN = [
  '30th Street Station', 'Suburban Station', 'Jefferson Station', 'Temple University',
  'North Philadelphia', 'University City', 'Airport Terminal A', 'Airport Terminal B',
  'Airport Terminal C-D', 'Airport Terminal E-F', 'Eastwick', 'Elwyn', 'Media',
  'Wynnewood', 'Paoli', 'Malvern', 'Downingtown', 'Thorndale', 'Ardmore', 'Bryn Mawr',
  'Villanova', 'Wayne', 'Norristown', 'Manayunk', 'Conshohocken', 'Cynwyd',
  'Chestnut Hill West', 'Chestnut Hill East', 'Fox Chase', 'Jenkintown-Wyncote',
  'Glenside', 'Warminster', 'Hatboro', 'Willow Grove', 'Lansdale', 'Doylestown',
  'Colmar', 'Fort Washington', 'Ambler', 'Trenton', 'Cornwells Heights',
  'Bristol', 'Levittown', 'Torresdale', 'Holmesburg Junction', 'Wilmington',
  'Newark', 'Marcus Hook', 'Chester', 'Highland Avenue', 'Gladstone', 'Wayne Jct',
  'West Trenton', 'Yardley', 'Woodbourne',
];

function railStations() {
  const custom = path.join(fixturesDir, 'rail_stations.json');
  if (fs.existsSync(custom)) {
    try { return JSON.parse(fs.readFileSync(custom, 'utf8')); } catch (e) { /* fall through */ }
  }
  return RAIL_STATIONS_BUILTIN;
}

/* ------------------------------ config store (DESIGN.md §6) ------------------------------ */

function defaultConfig() {
  return {
    version: 1,
    device: {
      name: 'transit-display',
      tz: 'EST5EDT,M3.2.0,M11.1.0',
      poll_seconds: 30,
      brightness: 80,
      rotation: 0,
      theme: 'light',
      invert_colors: false,
      ticker_lines: 3,
      ticker_speed: 30,
      ticker_show: 'both',
      logging: true,
      header: { name: false, clock: true, weather: true, wifi: true, updated: true },
      large_text: false,
      crowding: 'words',
      crowding_icons: 'seats',
      quiet: { enabled: false, start: '23:00', end: '06:00', brightness: 0, wake_seconds: 30 },
      night: { enabled: true, after_min: 60 },
    },
    stops: [
      { key: '17-21332', mode: 'bus', route: '17', stop_id: '21332', direction: '1', headsign: '20th-Johnston', label: '17 Southbound', stop_name: '19th St & Mifflin St', show: 3, lat: 39.927947, lng: -75.177147, title_style: 'label_dest', title_text: '', alt_of: '', alt_after_min: 15 },
      { key: '17-21297', mode: 'bus', route: '17', stop_id: '21297', direction: '0', headsign: '2nd-Market', label: '17 Northbound', stop_name: '20th St & Mifflin St', show: 3, title_style: 'label_dest', title_text: '', alt_of: '', alt_after_min: 15 },
      { key: 'rail-30th-N', mode: 'rail', station: '30th Street Station', direction: 'N', line: '', label: 'Regional Rail North', show: 2, title_style: 'label_dest', title_text: '', alt_of: '', alt_after_min: 15 },
    ],
    alerts: true,
    weather: { enabled: true, per_stop: true, units: 'f' },
    due: { enabled: true, minutes: 3, led: true, screen: true, chime: false },
    profiles: [],
    // Three default stations exercise every case the Now-page card needs to show:
    // Snyder & Dorrance mixes a 0 (classic), a 1-2 (ebikes) and a normal count (docks)
    // in one row; the other two are "no data" (missing from feed) and "offline".
    // See BIKE_DEMO below for the fixed counts.
    bike: {
      enabled: true,
      style: 'icons',
      stations: [
        { id: 3468, name: 'Snyder & Dorrance' },
        { id: 3005, name: '15th & Spruce' },
        { id: 3010, name: 'Girard Station' },
      ],
    },
  };
}

let config = defaultConfig();

const MODES = new Set(['bus', 'trolley', 'subway', 'rail']);

// Returns null if valid, else { error, path }.
function validateConfig(cfg) {
  if (typeof cfg !== 'object' || cfg === null) return { error: 'Config must be an object', path: '' };
  if (cfg.version !== 1) return { error: 'version must be 1', path: 'version' };
  const d = cfg.device;
  if (typeof d !== 'object' || d === null) return { error: 'device must be an object', path: 'device' };
  if (typeof d.name !== 'string' || !d.name.trim()) return { error: 'name must be a non-empty string', path: 'device.name' };
  if (typeof d.tz !== 'string' || !d.tz.trim()) return { error: 'tz must be a non-empty POSIX TZ string', path: 'device.tz' };
  if (!Number.isInteger(d.poll_seconds) || d.poll_seconds < 15 || d.poll_seconds > 120) {
    return { error: 'poll_seconds must be an integer between 15 and 120', path: 'device.poll_seconds' };
  }
  if (!Number.isInteger(d.brightness) || d.brightness < 10 || d.brightness > 100) {
    return { error: 'brightness must be an integer between 10 and 100', path: 'device.brightness' };
  }
  if (d.rotation !== undefined && ![0, 90, 180, 270].includes(d.rotation)) {
    return { error: 'rotation must be 0, 90, 180, or 270', path: 'device.rotation' };
  }
  if (d.theme !== undefined && !['light', 'dark'].includes(d.theme)) return { error: 'theme must be "light" or "dark"', path: 'device.theme' };
  if (d.invert_colors !== undefined && typeof d.invert_colors !== 'boolean') return { error: 'invert_colors must be a boolean', path: 'device.invert_colors' };
  if (d.ticker_lines !== undefined && (!Number.isInteger(d.ticker_lines) || d.ticker_lines < 1 || d.ticker_lines > 8)) {
    return { error: 'ticker_lines must be an integer between 1 and 8', path: 'device.ticker_lines' };
  }
  if (d.ticker_speed !== undefined && (!Number.isInteger(d.ticker_speed) || d.ticker_speed < 5 || d.ticker_speed > 200)) {
    return { error: 'ticker_speed must be an integer between 5 and 200', path: 'device.ticker_speed' };
  }
  if (d.ticker_show !== undefined && !['both', 'alerts', 'detours', 'off'].includes(d.ticker_show)) {
    return { error: 'ticker_show must be both, alerts, detours, or off', path: 'device.ticker_show' };
  }
  if (typeof d.logging !== 'boolean') return { error: 'logging must be a boolean', path: 'device.logging' };
  if (d.large_text !== undefined && typeof d.large_text !== 'boolean') return { error: 'large_text must be a boolean', path: 'device.large_text' };
  if (d.crowding !== undefined && !['off', 'words', 'icons', 'both'].includes(d.crowding)) {
    return { error: 'crowding must be off, words, icons, or both', path: 'device.crowding' };
  }
  if (d.crowding_icons !== undefined && !['seats', 'crowd'].includes(d.crowding_icons)) {
    return { error: 'crowding_icons must be seats or crowd', path: 'device.crowding_icons' };
  }
  // Note: legacy show_crowding (boolean) is accepted and folded onto crowding before
  // validateConfig ever runs (see the PUT /api/config handler), so it never reaches here.
  const TIME_RE = /^([01]\d|2[0-3]):[0-5]\d$/;
  if (d.quiet !== undefined) {
    const q = d.quiet;
    if (typeof q !== 'object' || q === null) return { error: 'quiet must be an object', path: 'device.quiet' };
    if (q.enabled !== undefined && typeof q.enabled !== 'boolean') return { error: 'enabled must be a boolean', path: 'device.quiet.enabled' };
    if (q.start !== undefined && (typeof q.start !== 'string' || !TIME_RE.test(q.start))) return { error: 'start must be "HH:MM"', path: 'device.quiet.start' };
    if (q.end !== undefined && (typeof q.end !== 'string' || !TIME_RE.test(q.end))) return { error: 'end must be "HH:MM"', path: 'device.quiet.end' };
    if (q.brightness !== undefined && (!Number.isInteger(q.brightness) || q.brightness < 0 || q.brightness > 50)) {
      return { error: 'brightness must be an integer between 0 and 50', path: 'device.quiet.brightness' };
    }
    if (q.wake_seconds !== undefined && (!Number.isInteger(q.wake_seconds) || q.wake_seconds < 5 || q.wake_seconds > 300)) {
      return { error: 'wake_seconds must be an integer between 5 and 300', path: 'device.quiet.wake_seconds' };
    }
  }
  if (d.night !== undefined) {
    const n = d.night;
    if (typeof n !== 'object' || n === null) return { error: 'night must be an object', path: 'device.night' };
    if (n.enabled !== undefined && typeof n.enabled !== 'boolean') return { error: 'enabled must be a boolean', path: 'device.night.enabled' };
    if (n.after_min !== undefined && (!Number.isInteger(n.after_min) || n.after_min < 15 || n.after_min > 240)) {
      return { error: 'after_min must be an integer between 15 and 240', path: 'device.night.after_min' };
    }
  }
  if (typeof cfg.alerts !== 'boolean') return { error: 'alerts must be a boolean', path: 'alerts' };
  if (d.header !== undefined) {
    if (typeof d.header !== 'object' || d.header === null) return { error: 'header must be an object', path: 'device.header' };
    for (const k of ['name', 'clock', 'weather', 'wifi', 'updated']) {
      if (d.header[k] !== undefined && typeof d.header[k] !== 'boolean') return { error: `${k} must be a boolean`, path: `device.header.${k}` };
    }
  }
  if (cfg.weather !== undefined) {
    const w = cfg.weather;
    if (typeof w !== 'object' || w === null) return { error: 'weather must be an object', path: 'weather' };
    if (w.enabled !== undefined && typeof w.enabled !== 'boolean') return { error: 'enabled must be a boolean', path: 'weather.enabled' };
    if (w.per_stop !== undefined && typeof w.per_stop !== 'boolean') return { error: 'per_stop must be a boolean', path: 'weather.per_stop' };
    if (w.units !== undefined && !['f', 'c'].includes(w.units)) return { error: 'units must be "f" or "c"', path: 'weather.units' };
  }
  if (cfg.due !== undefined) {
    const due = cfg.due;
    if (typeof due !== 'object' || due === null) return { error: 'due must be an object', path: 'due' };
    if (due.enabled !== undefined && typeof due.enabled !== 'boolean') return { error: 'enabled must be a boolean', path: 'due.enabled' };
    if (due.minutes !== undefined && (!Number.isInteger(due.minutes) || due.minutes < 1 || due.minutes > 15)) {
      return { error: 'minutes must be an integer between 1 and 15', path: 'due.minutes' };
    }
    for (const k of ['led', 'screen', 'chime']) {
      if (due[k] !== undefined && typeof due[k] !== 'boolean') return { error: `${k} must be a boolean`, path: `due.${k}` };
    }
  }
  if (cfg.bike !== undefined) {
    const bike = cfg.bike;
    if (typeof bike !== 'object' || bike === null) return { error: 'bike must be an object', path: 'bike' };
    if (bike.enabled !== undefined && typeof bike.enabled !== 'boolean') return { error: 'enabled must be a boolean', path: 'bike.enabled' };
    if (bike.style !== undefined && !['icons', 'words'].includes(bike.style)) {
      return { error: 'style must be "icons" or "words"', path: 'bike.style' };
    }
    if (bike.stations !== undefined) {
      if (!Array.isArray(bike.stations)) return { error: 'stations must be an array', path: 'bike.stations' };
      if (bike.stations.length > 3) return { error: 'Maximum of 3 stations', path: 'bike.stations' };
      for (let i = 0; i < bike.stations.length; i++) {
        const s = bike.stations[i];
        const p = `bike.stations[${i}]`;
        if (typeof s !== 'object' || s === null) return { error: 'station must be an object', path: p };
        if (!Number.isInteger(s.id) || s.id < 1) return { error: 'id must be an integer >= 1', path: `${p}.id` };
        if (typeof s.name !== 'string' || !s.name.trim()) return { error: 'name must be a non-empty string', path: `${p}.name` };
      }
    }
  }
  if (!Array.isArray(cfg.stops)) return { error: 'stops must be an array', path: 'stops' };
  if (cfg.stops.length > 8) return { error: 'Maximum of 8 stops', path: 'stops' };
  const seenKeys = new Set();
  for (let i = 0; i < cfg.stops.length; i++) {
    const s = cfg.stops[i];
    const p = `stops[${i}]`;
    if (typeof s !== 'object' || s === null) return { error: 'stop must be an object', path: p };
    if (typeof s.key !== 'string' || !s.key.trim()) return { error: 'key must be a non-empty string', path: `${p}.key` };
    if (seenKeys.has(s.key)) return { error: `duplicate key "${s.key}"`, path: `${p}.key` };
    seenKeys.add(s.key);
    if (!MODES.has(s.mode)) return { error: 'mode must be one of bus, trolley, subway, rail', path: `${p}.mode` };
    if (typeof s.label !== 'string' || !s.label.trim()) return { error: 'label must be a non-empty string', path: `${p}.label` };
    if (!Number.isInteger(s.show) || s.show < 1 || s.show > 4) return { error: 'show must be an integer between 1 and 4', path: `${p}.show` };
    if (s.mode === 'rail') {
      if (typeof s.station !== 'string' || !s.station.trim()) return { error: 'station must be a non-empty string', path: `${p}.station` };
      if (!['N', 'S', ''].includes(s.direction)) return { error: 'direction must be N, S, or "" (both)', path: `${p}.direction` };
    } else {
      if (typeof s.route !== 'string' || !s.route.trim()) return { error: 'route must be a non-empty string', path: `${p}.route` };
      if (typeof s.stop_id !== 'string' || !s.stop_id.trim()) return { error: 'stop_id must be a non-empty string', path: `${p}.stop_id` };
      if (!['0', '1'].includes(s.direction)) return { error: 'direction must be "0" or "1"', path: `${p}.direction` };
    }
  }
  // Second pass: fields added 2026-09-14 that reference other stops need the full key set above.
  const TITLE_STYLES = ['label_dest', 'label', 'route_dest_stop', 'custom'];
  for (let i = 0; i < cfg.stops.length; i++) {
    const s = cfg.stops[i];
    const p = `stops[${i}]`;
    if (s.title_style !== undefined && !TITLE_STYLES.includes(s.title_style)) {
      return { error: 'title_style must be one of label_dest, label, route_dest_stop, custom', path: `${p}.title_style` };
    }
    if (s.title_text !== undefined && (typeof s.title_text !== 'string' || s.title_text.length > 40)) {
      return { error: 'title_text must be a string of at most 40 characters', path: `${p}.title_text` };
    }
    if (s.alt_of !== undefined && s.alt_of !== '') {
      if (typeof s.alt_of !== 'string') return { error: 'alt_of must be a string', path: `${p}.alt_of` };
      if (s.alt_of === s.key) return { error: 'alt_of cannot reference its own stop', path: `${p}.alt_of` };
      if (!seenKeys.has(s.alt_of)) return { error: `alt_of references unknown stop "${s.alt_of}"`, path: `${p}.alt_of` };
    }
    if (s.alt_after_min !== undefined && (!Number.isInteger(s.alt_after_min) || s.alt_after_min < 5 || s.alt_after_min > 60)) {
      return { error: 'alt_after_min must be an integer between 5 and 60', path: `${p}.alt_after_min` };
    }
  }
  if (cfg.profiles !== undefined) {
    if (!Array.isArray(cfg.profiles)) return { error: 'profiles must be an array', path: 'profiles' };
    if (cfg.profiles.length > 4) return { error: 'Maximum of 4 profiles', path: 'profiles' };
    const timeRe = /^([01]\d|2[0-3]):[0-5]\d$/;
    for (let i = 0; i < cfg.profiles.length; i++) {
      const pr = cfg.profiles[i];
      const p = `profiles[${i}]`;
      if (typeof pr !== 'object' || pr === null) return { error: 'profile must be an object', path: p };
      if (typeof pr.name !== 'string' || !pr.name.trim() || pr.name.length > 24) {
        return { error: 'name must be a non-empty string of at most 24 characters', path: `${p}.name` };
      }
      if (!Array.isArray(pr.days)) return { error: 'days must be an array', path: `${p}.days` };
      const seenDays = new Set();
      for (const dNum of pr.days) {
        if (!Number.isInteger(dNum) || dNum < 0 || dNum > 6) return { error: 'days must be integers between 0 and 6', path: `${p}.days` };
        if (seenDays.has(dNum)) return { error: 'days must not repeat', path: `${p}.days` };
        seenDays.add(dNum);
      }
      if (typeof pr.start !== 'string' || !timeRe.test(pr.start)) return { error: 'start must be "HH:MM"', path: `${p}.start` };
      if (typeof pr.end !== 'string' || !timeRe.test(pr.end)) return { error: 'end must be "HH:MM"', path: `${p}.end` };
      if (!Array.isArray(pr.stops) || !pr.stops.length) return { error: 'stops must be a non-empty array of stop keys', path: `${p}.stops` };
      for (const key of pr.stops) {
        if (typeof key !== 'string' || !seenKeys.has(key)) return { error: `stops references unknown stop "${key}"`, path: `${p}.stops` };
      }
    }
  }
  return null;
}

/* ------------------------------ helpers ------------------------------ */

function sendJSON(res, status, obj) {
  const body = Buffer.from(JSON.stringify(obj));
  res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': body.length, 'Cache-Control': 'no-store' });
  res.end(body);
}
function sendText(res, status, text, type) {
  const body = Buffer.from(text);
  res.writeHead(status, { 'Content-Type': type || 'text/plain; charset=utf-8', 'Content-Length': body.length, 'Cache-Control': 'no-store' });
  res.end(body);
}
function notFound(res) { sendJSON(res, 404, { error: 'not found' }); }

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let len = 0;
    req.on('data', (c) => { chunks.push(c); len += c.length; });
    req.on('end', () => resolve({ buf: Buffer.concat(chunks, len), len }));
    req.on('error', reject);
  });
}

function stripHtml(s) {
  return String(s || '')
    .replace(/<[^>]*>/g, ' ')
    .replace(/&nbsp;/g, ' ')
    .replace(/&amp;/g, '&')
    .replace(/&#39;|&apos;/g, "'")
    .replace(/&quot;/g, '"')
    .replace(/\s+/g, ' ')
    .trim();
}

// Deterministic PRNG (mulberry32) seeded from a string, so /api/stats is stable per (stop, days).
function seedFrom(str) {
  let h = 1779033703 ^ str.length;
  for (let i = 0; i < str.length; i++) {
    h = Math.imul(h ^ str.charCodeAt(i), 3432918353);
    h = (h << 13) | (h >>> 19);
  }
  return () => {
    h = Math.imul(h ^ (h >>> 16), 2246822507);
    h = Math.imul(h ^ (h >>> 13), 3266489909);
    h ^= h >>> 16;
    return (h >>> 0) / 4294967296;
  };
}

function fmtSeptaTime(d) {
  let hh = d.getHours();
  const ap = hh >= 12 ? 'p' : 'a';
  hh = hh % 12; if (hh === 0) hh = 12;
  const mm = String(d.getMinutes()).padStart(2, '0');
  return `${hh}:${mm}${ap}`;
}
function fmtDateCalendar(d) {
  const mm = String(d.getMonth() + 1).padStart(2, '0');
  const dd = String(d.getDate()).padStart(2, '0');
  const yy = String(d.getFullYear()).slice(-2);
  let hh = d.getHours();
  const ap = hh >= 12 ? 'pm' : 'am';
  hh = hh % 12; if (hh === 0) hh = 12;
  const mi = String(d.getMinutes()).padStart(2, '0');
  return `${mm}/${dd}/${yy} ${hh}:${mi} ${ap}`;
}
const DAY_ABBR = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

/* ------------------------------ /api/state ------------------------------ */

function buildState(query) {
  const now = Math.floor(Date.now() / 1000);
  const stale = query.get('stale') === '1';

  const stopsOut = config.stops.map((cfgStop) => {
    if (cfgStop.mode === 'rail') return buildRailSnapshot(cfgStop, now);
    return buildBusSnapshot(cfgStop, now);
  });

  const rawAlert = FIX.alerts17[0];
  const alerts = [];
  if (config.alerts && rawAlert) {
    const detourTexts = [];
    const seen = new Set();
    for (const dt of rawAlert.detour || []) {
      const text = `${dt.location_start}${dt.location_end ? ' to ' + dt.location_end : ''}: ${stripHtml(dt.message)} (${stripHtml(dt.reason)})`;
      if (seen.has(text)) continue;
      seen.add(text);
      detourTexts.push(text);
    }
    alerts.push({
      route: rawAlert.route,
      text: stripHtml(rawAlert.advisory || rawAlert.description || '').slice(0, 240),
      detours: detourTexts,
      current: true,
    });
  }

  return {
    time: now,
    uptime: Math.floor(process.uptime()),
    heap: 142000 + Math.round(Math.sin(now / 37) * 8000),
    wifi: { ssid: 'MockWiFi-5G', rssi: -58, ip: '192.168.1.42', mdns: `${config.device.name}.local` },
    sd: { mounted: true, free_mb: 7423, log_bytes: 128933 },
    last_poll: stale
      ? { ok: false, age_s: 420, error: 'SEPTA request timed out' }
      : { ok: true, age_s: 5 + (now % 20), error: '' },
    firmware_version: '0.1.0-mock',
    stops: stopsOut,
    alerts,
    weather: buildWeather(now),
    active_profile: activeProfileName(now),
    bike: buildBike(now),
  };
}

// Local "HH:MM" for a profile's days/start/end window check.
function hhmmLocal(now) {
  const d = new Date(now * 1000);
  return `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}`;
}
// Window may cross midnight (matches how device.quiet is documented in DESIGN.md §6).
function timeInWindow(cur, start, end) {
  if (start === end) return true;
  if (start < end) return cur >= start && cur < end;
  return cur >= start || cur < end;
}
// Name of the first configured profile whose days/time window contains "now", else "".
function activeProfileName(now) {
  const dow = new Date(now * 1000).getDay();
  const cur = hhmmLocal(now);
  for (const p of config.profiles || []) {
    if (!p.days || !p.days.includes(dow)) continue;
    if (!timeInWindow(cur, p.start, p.end)) continue;
    return p.name;
  }
  return '';
}

// Fixed counts for the three default stations (see defaultConfig above) so the web UI's
// Indego card can be checked for every case — a 0, a 1-2, a normal count, a station
// missing from the feed (-1s), and an offline one — without waiting on the real feed.
// Stations a user adds beyond these still get randomized counts below.
const BIKE_DEMO = {
  3468: { classic: 0, ebikes: 2, docks: 11, total_docks: 15, active: true },
  3005: { missing: true, active: true },
  3010: { missing: true, active: false },
};

// Mock Indego bikes/docks for the configured stations (DESIGN.md §4.9's state.bike shape).
function buildBike(now) {
  const b = config.bike || {};
  if (!b.enabled || !b.stations || !b.stations.length) return { enabled: !!b.enabled, age_s: 0, stations: [] };
  const rand = seedFrom(`bike|${Math.floor(now / 300)}`);
  const stations = b.stations.map((s) => {
    const demo = BIKE_DEMO[s.id];
    if (demo) {
      if (demo.missing) {
        return { id: s.id, name: s.name, bikes: -1, classic: -1, ebikes: -1, docks: -1, total_docks: -1, active: demo.active };
      }
      const bikes = demo.classic + demo.ebikes;
      return { id: s.id, name: s.name, bikes, classic: demo.classic, ebikes: demo.ebikes, docks: demo.docks, total_docks: demo.total_docks, active: demo.active };
    }
    const classic = Math.floor(rand() * 10);
    const ebikes = Math.floor(rand() * 4);
    const docks = 5 + Math.floor(rand() * 15);
    const total_docks = docks + classic + ebikes;
    return { id: s.id, name: s.name, bikes: classic + ebikes, classic, ebikes, docks, total_docks, active: true };
  });
  return { enabled: true, age_s: 30 + Math.round(now % 120), stations };
}

// Mirrors firmware/src/app/weather_service.cpp: main location conditions plus six hourly slots.
function buildWeather(now) {
  const w = config.weather || {};
  if (w.enabled === false) return { enabled: false, units: w.units || 'f', age_s: -1 };
  const hourStart = now - (now % 3600);
  const codes = [1, 2, 3, 61, 63, 80];
  const text = { 1: 'mostly clear', 2: 'partly cloudy', 3: 'overcast', 61: 'light rain', 63: 'rain', 80: 'light showers' };
  const f = (w.units || 'f') === 'f';
  return {
    enabled: true, units: f ? 'f' : 'c', age_s: 60 + (now % 300),
    main: {
      temp: f ? 69.4 : 20.8, feels_like: f ? 68.7 : 20.4, code: 1, text: 'mostly clear', wind: 9.3,
      hours: codes.map((code, i) => ({ t: hourStart + i * 3600, code, text: text[code], prob: [0, 5, 20, 60, 80, 55][i], temp: (f ? 69 : 20.5) + i })),
    },
  };
}

function weatherNoteFor(cfgStop) {
  const w = config.weather || {};
  if (w.enabled === false || w.per_stop === false) return '';
  return cfgStop.direction === '0' ? 'rain likely (60%) at 10:15a' : '';
}

function buildBusSnapshot(cfgStop, now) {
  const bus17 = FIX.transitview17.bus || [];
  const isSouth = cfgStop.direction === '1';
  const vehicle = bus17.find((b) => (isSouth ? b.Direction === 'Southbound' : b.Direction === 'Northbound'));
  const sched = cfgStop.stop_id === '21332' ? FIX.sched21332 : cfgStop.stop_id === '21297' ? FIX.sched21297 : null;
  const schedEntries = (sched && sched[cfgStop.route]) || [];

  const arrivals = [];
  if (vehicle) {
    const scheduled = now + 300;
    const predicted = scheduled + (vehicle.late || 0) * 60;
    arrivals.push({
      trip: vehicle.trip, vehicle: vehicle.VehicleID, destination: vehicle.destination,
      predicted, scheduled, eta_s: predicted - now,
      late_min: vehicle.late || 0, late_known: true, status: 'live', seats: vehicle.estimated_seat_availability || '',
    });
  }
  // Synthetic seats values on the scheduled/skipped slots (SEPTA doesn't send crowding
  // for these anyway) so the Now page's crowding UI can be exercised end to end: the two
  // configured bus stops' six slots together cover every seats value the firmware knows
  // about (the live slot's value comes from the transitview fixture — EMPTY southbound,
  // FEW_SEATS_AVAILABLE northbound).
  const schedFallback = schedEntries[0] || {};
  arrivals.push({
    trip: schedFallback.trip_id || '000000', vehicle: '', destination: schedFallback.DirectionDesc || cfgStop.headsign,
    predicted: 0, scheduled: now + 1500, eta_s: 1500, late_min: 0, late_known: false, status: 'scheduled',
    seats: isSouth ? 'MANY_SEATS_AVAILABLE' : 'STANDING_ROOM_ONLY',
  });
  const lastTrip = schedEntries[schedEntries.length - 1] || {};
  arrivals.push({
    trip: lastTrip.trip_id || '000001', vehicle: '', destination: lastTrip.DirectionDesc || cfgStop.headsign,
    predicted: 0, scheduled: now + 2200, eta_s: 2200, late_min: 0, late_known: false, status: 'skipped',
    seats: isSouth ? 'FULL' : 'CRUSHED_STANDING_ROOM_ONLY',
  });
  arrivals.sort((a, b) => (a.predicted || a.scheduled) - (b.predicted || b.scheduled));

  return {
    key: cfgStop.key, mode: cfgStop.mode, route: cfgStop.route, stop_id: cfgStop.stop_id,
    direction: cfgStop.direction, headsign: cfgStop.headsign, label: cfgStop.label,
    stop_name: cfgStop.stop_name, show: cfgStop.show, ok: true, error: '', weather_note: weatherNoteFor(cfgStop), fetched: now, arrivals,
  };
}

function buildRailSnapshot(cfgStop, now) {
  const key = Object.keys(FIX.arrivals30th)[0];
  const sections = (key && FIX.arrivals30th[key]) || [];
  const wantKey = cfgStop.direction === 'S' ? 'Southbound' : 'Northbound';
  let trains = [];
  for (const sec of sections) if (sec[wantKey]) trains = sec[wantKey];
  if (!trains.length && cfgStop.direction === '') {
    for (const sec of sections) for (const arr of Object.values(sec)) trains = trains.concat(arr);
  }

  const arrivals = trains.slice(0, cfgStop.show || 2).map((t, i) => {
    const sched = new Date(t.sched_time.replace(' ', 'T') + 'Z').getTime();
    const dep = new Date(t.depart_time.replace(' ', 'T') + 'Z').getTime();
    const delaySec = Math.round((dep - sched) / 1000);
    const scheduled = now + 240 + i * 480;
    const predicted = scheduled + delaySec;
    // Regional Rail doesn't carry SEPTA's bus-style seat estimate; NOT_AVAILABLE on the
    // first train and '' (blank) on the rest both map to "no crowding element" on the Now
    // page, but NOT_AVAILABLE also exercises that legacy/unknown-string code path.
    return {
      trip: t.train_id, vehicle: t.train_id, destination: t.destination,
      predicted, scheduled, eta_s: predicted - now,
      late_min: Math.round(delaySec / 60), late_known: true, status: 'live', seats: i === 0 ? 'NOT_AVAILABLE' : '',
    };
  });

  return {
    key: cfgStop.key, mode: 'rail', station: cfgStop.station, direction: cfgStop.direction,
    headsign: '', label: cfgStop.label, stop_name: cfgStop.station, show: cfgStop.show,
    ok: true, error: '', weather_note: '', fetched: now, arrivals,
  };
}

/* ------------------------------ /api/proxy/* ------------------------------ */

function proxyStops(route) {
  if (route === '17') return FIX.stops17;
  if (route === '34') return FIX.stops34;
  if (route === 'BSL') return FIX.stopsBSL;
  return [];
}

function proxySchedule(stopId) {
  if (stopId === '21332') return FIX.sched21332;
  if (stopId === '21297') return FIX.sched21297;
  // Synthesize a plausible response so the wizard flow works for other stop_ids in dev.
  const hit = (FIX.stops17 || []).find((s) => s.stopid === stopId);
  const stopName = hit ? hit.stopname : `Stop ${stopId}`;
  const now = Date.now();
  const mk = (mins, dir, desc) => {
    const d = new Date(now + mins * 60000);
    return { StopName: stopName, Route: '17', trip_id: String(900000 + mins), date: fmtSeptaTime(d), day: DAY_ABBR[d.getDay()], Direction: dir, DateCalender: fmtDateCalendar(d), DirectionDesc: desc };
  };
  return { 17: [mk(6, '1', '20th-Johnston'), mk(14, '1', '20th-Johnston'), mk(9, '0', '2nd-Market'), mk(21, '0', '2nd-Market')] };
}

/* ------------------------------ /api/stats ------------------------------ */

// Crowding levels: 0 empty, 1 open, 2 few seats, 3 standing, 4 packed, 5 full (matches
// the word/icon mapping the Now page already uses for SEPTA's seat estimate, see
// CROWD_WORDS in app.js). Rush hours skew the per-sample level distribution higher.
function crowdSample(rand, rush) {
  const r = rand();
  if (rush) return r < 0.05 ? 0 : r < 0.15 ? 1 : r < 0.35 ? 2 : r < 0.6 ? 3 : r < 0.85 ? 4 : 5;
  return r < 0.25 ? 0 : r < 0.55 ? 1 : r < 0.8 ? 2 : r < 0.93 ? 3 : r < 0.98 ? 4 : 5;
}
function crowdBin(n, rush, rand) {
  const dist = [0, 0, 0, 0, 0, 0];
  for (let i = 0; i < n; i++) dist[crowdSample(rand, rush)]++;
  const mean = +(dist.reduce((a, c, lvl) => a + c * lvl, 0) / n).toFixed(2);
  return { n, mean, dist };
}
function buildCrowdingByHour(rand, scale) {
  const out = [];
  for (let hh = 5; hh <= 23; hh++) {
    const rush = (hh >= 7 && hh <= 9) || (hh >= 16 && hh <= 18);
    const n = Math.round((rush ? 16 : 5) * scale * (0.6 + rand()));
    if (n <= 0) continue;
    out.push({ h: hh, ...crowdBin(n, rush, rand) });
  }
  return out;
}
function buildCrowdingByWeekday(rand, scale) {
  const out = [];
  for (let wd = 0; wd < 7; wd++) {
    const weekend = wd === 0 || wd === 6;
    const n = Math.round((weekend ? 30 : 70) * scale * (0.6 + rand()));
    if (n <= 0) continue;
    out.push({ wd, ...crowdBin(n, !weekend, rand) });
  }
  return out;
}
// Gap between consecutive buses, by hour of day. mean_gap_s/max_gap_s pair with the
// same n as by_hour above so "typical wait ~ half the mean gap" lines up with the
// lateness chart's sample counts.
function buildWaitByHour(rand, scale) {
  const out = [];
  for (let hh = 5; hh <= 23; hh++) {
    const rush = (hh >= 7 && hh <= 9) || (hh >= 16 && hh <= 18);
    const n = Math.round((rush ? 16 : 5) * scale * (0.6 + rand()));
    if (n <= 0) continue;
    const mean_gap_s = Math.round((rush ? 360 : 600) * (0.7 + rand() * 0.6));
    const max_gap_s = Math.round(mean_gap_s * (1.5 + rand() * 1.5));
    const ghost = Math.round(rand() * (rush ? 1.2 : 0.4));
    const noshow = Math.round(rand() * (rush ? 0.6 : 0.3));
    out.push({ h: hh, n, mean_gap_s, max_gap_s, ghost, noshow });
  }
  return out;
}

function buildStats(stop, days, empty) {
  if (empty) {
    return {
      stop, days, samples: 0, on_time_pct: 0, mean_late_min: 0,
      by_hour: [], by_weekday: [], headway: { n: 0, bunched: 0, gapped: 0, ratio_hist: [] },
      ghost: 0, noshow: 0, outage_min: 0, prediction: [],
      crowding: { by_hour: [], by_weekday: [] }, wait_by_hour: [],
    };
  }
  const rand = seedFrom(`${stop}|${days}`);
  const scale = days === 7 ? 0.25 : days === 90 ? 3 : 1;

  const by_hour = [];
  for (let hh = 5; hh <= 23; hh++) {
    const rush = (hh >= 7 && hh <= 9) || (hh >= 16 && hh <= 18);
    const n = Math.round((rush ? 18 : 6) * scale * (0.6 + rand()));
    if (n <= 0) continue;
    const mean = +(rush ? 6 + rand() * 8 : 1 + rand() * 5).toFixed(1);
    const p50 = Math.max(0, Math.round(mean - 1 + rand() * 2));
    const p90 = Math.round(mean + 4 + rand() * 10);
    by_hour.push({ h: hh, n, mean, p50, p90 });
  }

  const by_weekday = [];
  for (let d = 0; d < 7; d++) {
    const weekend = d === 0 || d === 6;
    const n = Math.round((weekend ? 40 : 90) * scale * (0.6 + rand()));
    const mean = +((weekend ? 2 : 4) + rand() * 5).toFixed(1);
    const p50 = Math.max(0, Math.round(mean - 1));
    const p90 = Math.round(mean + 8 + rand() * 6);
    by_weekday.push({ d, n, mean, p50, p90 });
  }

  const totalN = by_hour.reduce((a, b) => a + b.n, 0) || 1;
  const onTimeShare = 0.6 + rand() * 0.25;
  const samples = totalN;
  const on_time_pct = +(onTimeShare * 100).toFixed(1);
  const mean_late_min = +(by_hour.reduce((a, b) => a + b.mean * b.n, 0) / totalN).toFixed(1);

  const headwayN = Math.round(samples * 0.9);
  const ratio_hist = [
    { pct_lo: 0, pct_hi: 40 }, { pct_lo: 40, pct_hi: 80 }, { pct_lo: 80, pct_hi: 120 },
    { pct_lo: 120, pct_hi: 175 }, { pct_lo: 175, pct_hi: 999 },
  ].map((bucket, i) => {
    const weight = [0.12, 0.28, 0.32, 0.18, 0.10][i];
    return { ...bucket, bucket: `${bucket.pct_lo}-${bucket.pct_hi >= 999 ? '999+' : bucket.pct_hi}%`, count: Math.round(headwayN * weight * (0.7 + rand() * 0.6)) };
  });
  const bunched = ratio_hist[0].count;
  const gapped = ratio_hist[ratio_hist.length - 1].count;

  const prediction = [120, 300, 600, 900].map((horizon_s) => ({
    horizon_s,
    n: Math.round(samples * 0.7 * (0.8 + rand() * 0.4)),
    mae_s: Math.round(30 + (horizon_s / 60) * (4 + rand() * 6)),
    bias_s: Math.round((rand() - 0.5) * (20 + horizon_s / 15)),
  }));

  return {
    stop, days, samples, on_time_pct, mean_late_min, by_hour, by_weekday,
    headway: { n: headwayN, bunched, gapped, ratio_hist },
    ghost: Math.round(3 * scale * rand() + 1),
    noshow: Math.round(2 * scale * rand()),
    outage_min: Math.round(20 * scale * rand()),
    prediction,
    crowding: { by_hour: buildCrowdingByHour(rand, scale), by_weekday: buildCrowdingByWeekday(rand, scale) },
    wait_by_hour: buildWaitByHour(rand, scale),
  };
}

/* ------------------------------ /api/stats/overview ------------------------------ */

// Fraction of total dock capacity occupied by bikes, by hour of day: high overnight
// (bikes come home), draining through the AM commute to a trough around 8-9a, then
// slowly refilling through the day and evening as riders return them.
const BIKE_HOURLY_FRACTION = [
  0.86, 0.85, 0.83, 0.79, 0.70, 0.53, 0.36, 0.20, 0.12, 0.16, 0.25, 0.34,
  0.40, 0.43, 0.46, 0.50, 0.55, 0.61, 0.69, 0.76, 0.81, 0.84, 0.86, 0.87,
];
function buildBikeByHour(stationId, days) {
  const rand = seedFrom(`bikeoverview|${stationId}|${days}`);
  const scale = days === 7 ? 0.25 : days === 90 ? 3 : 1;
  const totalDocks = 12 + Math.floor(rand() * 8); // 12-19, fixed per station via the seed
  const out = [];
  for (let hh = 0; hh < 24; hh++) {
    const n = Math.max(1, Math.round((6 + rand() * 6) * scale));
    const bikes = Math.min(totalDocks - 1, Math.max(0, +(totalDocks * BIKE_HOURLY_FRACTION[hh] * (0.85 + rand() * 0.3)).toFixed(1)));
    const ebikes = Math.min(bikes, +(bikes * (0.2 + rand() * 0.25)).toFixed(1));
    const docks = +Math.max(0, totalDocks - bikes).toFixed(1);
    out.push({ h: hh, n, bikes, ebikes, docks });
  }
  return out;
}
function buildBikesOverview(days) {
  const b = config.bike || {};
  if (!b.enabled || !b.stations || !b.stations.length) return [];
  return b.stations.map((st) => ({ station: `indego-${st.id}`, name: st.name, by_hour: buildBikeByHour(st.id, days) }));
}

// Reuses buildStats' own PRNG sequence for the summary numbers so a stop's overview row
// always matches the tiles shown when you select it below, then adds a last_seen_ts the
// per-stop endpoint has no reason to carry.
function buildStatsOverview(days) {
  const now = Math.floor(Date.now() / 1000);
  const stops = (config.stops || []).map((s) => {
    const full = buildStats(s.key, days, false);
    const seenRand = seedFrom(`overview-seen|${s.key}|${days}`);
    return {
      stop: s.key, samples: full.samples, on_time_pct: full.on_time_pct, mean_late_min: full.mean_late_min,
      ghost: full.ghost, noshow: full.noshow, last_seen_ts: now - Math.round(seenRand() * 10800),
    };
  });
  return { days, stops, bikes: buildBikesOverview(days) };
}

/* ------------------------------ /api/log/* ------------------------------ */

const CSV_HEADER = 'ts,event,stop_key,route,dir,trip,vehicle,scheduled_ts,predicted_ts,actual_ts,late_min,horizon_s,headway_s,note';

function logIndex() {
  const now = new Date();
  const thisMonth = `${now.getFullYear()}-${String(now.getMonth() + 1).padStart(2, '0')}.csv`;
  const prev = new Date(now.getFullYear(), now.getMonth() - 1, 1);
  const prevMonth = `${prev.getFullYear()}-${String(prev.getMonth() + 1).padStart(2, '0')}.csv`;
  return [{ file: prevMonth, bytes: 214532 }, { file: thisMonth, bytes: 46980 }];
}

function logCsv(file) {
  if (!/^\d{4}-\d{2}\.csv$/.test(file)) return null;
  const now = Math.floor(Date.now() / 1000);
  const rows = [
    `${now - 3600},pred,17-21332,17,1,3667,7477,${now - 3300},${now - 3200},,13,900,,`,
    `${now - 3000},pred,17-21332,17,1,3667,7477,${now - 3300},${now - 2900},,11,600,,`,
    `${now - 2600},arrive,17-21332,17,1,3667,7477,${now - 3300},${now - 2610},${now - 2600},12,,540,`,
    `${now - 2000},pred,17-21297,17,0,3585,7300,${now - 1900},${now - 1850},,1,300,,`,
    `${now - 1500},arrive,17-21297,17,0,3585,7300,${now - 1900},${now - 1510},${now - 1500},1,,600,`,
    `${now - 900},ghost,17-21332,17,1,3654,7481,${now - 700},${now - 690},,,,,vanished before arriving`,
    `${now - 400},noshow,17-21297,17,0,,,${now - 500},,,,,,` ,
    `${now - 100},outage,17-21332,17,1,,,,,,,,,SEPTA API unreachable 6 min`,
  ];
  return [CSV_HEADER, ...rows].join('\n') + '\n';
}

/* ------------------------------ static files ------------------------------ */

const STATIC = {
  '/': { file: 'index.html', type: 'text/html; charset=utf-8' },
  '/index.html': { file: 'index.html', type: 'text/html; charset=utf-8' },
  '/app.js': { file: 'app.js', type: 'application/javascript; charset=utf-8' },
  '/app.css': { file: 'app.css', type: 'text/css; charset=utf-8' },
  '/favicon.svg': { file: 'favicon.svg', type: 'image/svg+xml' },
};

/* ------------------------------ server ------------------------------ */

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || 'localhost'}`);
  const { pathname } = url;
  const q = url.searchParams;

  try {
    if (STATIC[pathname] && req.method === 'GET') {
      const spec = STATIC[pathname];
      const filePath = path.join(webDir, spec.file);
      const body = fs.readFileSync(filePath);
      res.writeHead(200, { 'Content-Type': spec.type, 'Content-Length': body.length, 'Cache-Control': 'no-store' });
      res.end(body);
      return;
    }

    if (pathname === '/api/state' && req.method === 'GET') return sendJSON(res, 200, buildState(q));

    if (pathname === '/api/config' && req.method === 'GET') return sendJSON(res, 200, config);
    if (pathname === '/api/config' && req.method === 'PUT') {
      const { buf } = await readBody(req);
      let parsed;
      try { parsed = JSON.parse(buf.toString('utf8') || '{}'); } catch (e) { return sendJSON(res, 400, { error: 'Invalid JSON body', path: '' }); }
      // Legacy clients send show_crowding (boolean) instead of crowding (string). Mirror
      // the real firmware: accept it on read, fold it onto crowding, never persist it.
      if (parsed && typeof parsed.device === 'object' && parsed.device !== null) {
        const pd = parsed.device;
        if (pd.crowding === undefined && pd.show_crowding !== undefined) {
          pd.crowding = pd.show_crowding === false ? 'off' : 'words';
        }
        delete pd.show_crowding;
        // HTTPS/TLS controls removed from the firmware and the Settings page (task C).
        // An old client may still send use_https/tls_verify — accept the request rather
        // than 400ing on them, but drop the keys so they don't get persisted or echoed
        // back on the next GET /api/config.
        delete pd.use_https;
        delete pd.tls_verify;
      }
      const err = validateConfig(parsed);
      if (err) return sendJSON(res, 400, err);
      config = parsed;
      return sendJSON(res, 200, config);
    }

    if (pathname === '/api/proxy/stops' && req.method === 'GET') return sendJSON(res, 200, proxyStops(q.get('route') || ''));
    if (pathname === '/api/proxy/schedule' && req.method === 'GET') return sendJSON(res, 200, proxySchedule(q.get('stop_id') || ''));
    if (pathname === '/api/rail/stations' && req.method === 'GET') return sendJSON(res, 200, railStations());

    if (pathname === '/api/stats' && req.method === 'GET') {
      const stop = q.get('stop');
      if (!stop) return sendJSON(res, 400, { error: 'stop is required', path: 'stop' });
      let days = Number(q.get('days') || 30);
      if (![7, 30, 90].includes(days)) days = [7, 30, 90].reduce((a, b) => (Math.abs(b - days) < Math.abs(a - days) ? b : a));
      return sendJSON(res, 200, buildStats(stop, days, q.get('empty') === '1'));
    }
    if (pathname === '/api/stats/overview' && req.method === 'GET') {
      let days = Number(q.get('days') || 30);
      if (![7, 30, 90].includes(days)) days = [7, 30, 90].reduce((a, b) => (Math.abs(b - days) < Math.abs(a - days) ? b : a));
      return sendJSON(res, 200, buildStatsOverview(days));
    }

    if (pathname === '/api/log/index' && req.method === 'GET') return sendJSON(res, 200, logIndex());
    if (pathname.startsWith('/api/log/') && req.method === 'GET') {
      const file = decodeURIComponent(pathname.slice('/api/log/'.length));
      const csv = logCsv(file);
      if (!csv) return notFound(res);
      return sendText(res, 200, csv, 'text/csv; charset=utf-8');
    }

    if (pathname === '/api/ota' && req.method === 'POST') {
      const { len } = await readBody(req);
      const delay = Math.min(1500, 300 + Math.round(len / 20000));
      setTimeout(() => sendJSON(res, 200, { ok: true, bytes: len }), delay);
      return;
    }
    if (pathname === '/api/reboot' && req.method === 'POST') return sendJSON(res, 200, { ok: true });
    if (pathname === '/api/wifi/reset' && req.method === 'POST') return sendJSON(res, 200, { ok: true });

    notFound(res);
  } catch (e) {
    sendJSON(res, 500, { error: e.message || 'internal error' });
  }
});

server.listen(PORT, () => {
  console.log(`Philly Transit Display mock server listening on http://localhost:${PORT}`);
});

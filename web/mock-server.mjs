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
      use_https: false,
      tls_verify: true,
      logging: true,
    },
    stops: [
      { key: '17-21332', mode: 'bus', route: '17', stop_id: '21332', direction: '1', headsign: '20th-Johnston', label: '17 Southbound', stop_name: '19th St & Mifflin St', show: 3 },
      { key: '17-21297', mode: 'bus', route: '17', stop_id: '21297', direction: '0', headsign: '2nd-Market', label: '17 Northbound', stop_name: '20th St & Mifflin St', show: 3 },
      { key: 'rail-30th-N', mode: 'rail', station: '30th Street Station', direction: 'N', line: '', label: 'Regional Rail North', show: 2 },
    ],
    alerts: true,
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
  if (d.use_https !== undefined && typeof d.use_https !== 'boolean') return { error: 'use_https must be a boolean', path: 'device.use_https' };
  if (d.theme !== undefined && !['light', 'dark'].includes(d.theme)) return { error: 'theme must be "light" or "dark"', path: 'device.theme' };
  if (d.invert_colors !== undefined && typeof d.invert_colors !== 'boolean') return { error: 'invert_colors must be a boolean', path: 'device.invert_colors' };
  if (d.ticker_lines !== undefined && (!Number.isInteger(d.ticker_lines) || d.ticker_lines < 1 || d.ticker_lines > 8)) {
    return { error: 'ticker_lines must be an integer between 1 and 8', path: 'device.ticker_lines' };
  }
  if (d.ticker_speed !== undefined && (!Number.isInteger(d.ticker_speed) || d.ticker_speed < 5 || d.ticker_speed > 200)) {
    return { error: 'ticker_speed must be an integer between 5 and 200', path: 'device.ticker_speed' };
  }
  if (typeof d.tls_verify !== 'boolean') return { error: 'tls_verify must be a boolean', path: 'device.tls_verify' };
  if (typeof d.logging !== 'boolean') return { error: 'logging must be a boolean', path: 'device.logging' };
  if (typeof cfg.alerts !== 'boolean') return { error: 'alerts must be a boolean', path: 'alerts' };
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
  };
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
  const schedFallback = schedEntries[0] || {};
  arrivals.push({
    trip: schedFallback.trip_id || '000000', vehicle: '', destination: schedFallback.DirectionDesc || cfgStop.headsign,
    predicted: 0, scheduled: now + 1500, eta_s: 1500, late_min: 0, late_known: false, status: 'scheduled', seats: '',
  });
  const lastTrip = schedEntries[schedEntries.length - 1] || {};
  arrivals.push({
    trip: lastTrip.trip_id || '000001', vehicle: '', destination: lastTrip.DirectionDesc || cfgStop.headsign,
    predicted: 0, scheduled: now + 2200, eta_s: 2200, late_min: 0, late_known: false, status: 'skipped', seats: '',
  });
  arrivals.sort((a, b) => (a.predicted || a.scheduled) - (b.predicted || b.scheduled));

  return {
    key: cfgStop.key, mode: cfgStop.mode, route: cfgStop.route, stop_id: cfgStop.stop_id,
    direction: cfgStop.direction, headsign: cfgStop.headsign, label: cfgStop.label,
    stop_name: cfgStop.stop_name, show: cfgStop.show, ok: true, error: '', fetched: now, arrivals,
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
    return {
      trip: t.train_id, vehicle: t.train_id, destination: t.destination,
      predicted, scheduled, eta_s: predicted - now,
      late_min: Math.round(delaySec / 60), late_known: true, status: 'live', seats: '',
    };
  });

  return {
    key: cfgStop.key, mode: 'rail', station: cfgStop.station, direction: cfgStop.direction,
    headsign: '', label: cfgStop.label, stop_name: cfgStop.station, show: cfgStop.show,
    ok: true, error: '', fetched: now, arrivals,
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

function buildStats(stop, days, empty) {
  if (empty) {
    return {
      stop, days, samples: 0, on_time_pct: 0, mean_late_min: 0,
      by_hour: [], by_weekday: [], headway: { n: 0, bunched: 0, gapped: 0, ratio_hist: [] },
      ghost: 0, noshow: 0, outage_min: 0, prediction: [],
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
  };
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

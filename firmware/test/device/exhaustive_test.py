#!/usr/bin/env python3
"""Exhaustive on-device test of the Philly Transit Display firmware over its HTTP API.
Backs up the owner's config first and restores it at the end (with invert_colors=false)."""
import json, subprocess, sys, time, threading, re, copy, datetime, concurrent.futures, os
B = 'http://192.168.1.181'
S = os.path.dirname(os.path.abspath(__file__))

# The admin PIN protects every state-changing endpoint (DESIGN.md SS7/SS12). It is per device and
# never in the repo: read it off the serial console at boot ("[auth] web PIN: 123456") or the
# device info screen, then run this script with CYD_PIN=123456.
PIN = os.environ.get('CYD_PIN', '').strip()
if not PIN:
    sys.exit('CYD_PIN is not set. Read the PIN from the serial console at boot ("[auth] web PIN: ...")\n'
             'or from the device info screen on the panel, then run:  CYD_PIN=123456 %s' % sys.argv[0])
PINH = ['-H', 'X-Pin: ' + PIN]
results = []
serial_lines = []
stop_serial = False

def curl(args, timeout=20):
    r = subprocess.run(['curl', '-s', '--max-time', str(timeout)] + args, capture_output=True)
    return r

def get(path, timeout=15, pin=False):
    r = curl((PINH if pin else []) + ['-w', '\n%{http_code}', B + path], timeout)
    body, _, code = r.stdout.rpartition(b'\n')
    return int(code or 0), body

def get_json(path):
    code, body = get(path)
    try: return code, json.loads(body)
    except Exception: return code, None

def put_cfg(cfg, pin=PIN):
    r = curl(['-o', '/dev/null', '-w', '%{http_code}', '-X', 'PUT', '-H', 'X-Pin: ' + pin, '-H', 'Content-Type: application/json', '--data-binary', json.dumps(cfg), B + '/api/config'])
    return int(r.stdout or 0)

def put_cfg_nopin(cfg):
    r = curl(['-w', '\n%{http_code}', '-X', 'PUT', '-H', 'Content-Type: application/json', '--data-binary', json.dumps(cfg), B + '/api/config'])
    body, _, code = r.stdout.rpartition(b'\n')
    try: return int(code or 0), json.loads(body)
    except Exception: return int(code or 0), None

def put_cfg_body(cfg):
    r = curl(PINH + ['-w', '\n%{http_code}', '-X', 'PUT', '-H', 'Content-Type: application/json', '--data-binary', json.dumps(cfg), B + '/api/config'])
    body, _, code = r.stdout.rpartition(b'\n')
    try: return int(code or 0), json.loads(body)
    except Exception: return int(code or 0), None

def post(path):
    r = curl(PINH + ['-o', '/dev/null', '-w', '%{http_code}', '-X', 'POST', B + path])
    return int(r.stdout or 0)

def check(name, ok, detail=''):
    results.append((name, bool(ok), detail))
    print(('PASS ' if ok else 'FAIL ') + name + (('  -- ' + str(detail)) if detail and not ok else ''), flush=True)

def ui():
    code, d = get_json('/api/debug/ui')
    return d or {}

def state():
    code, d = get_json('/api/state')
    return d or {}

def config():
    code, d = get_json('/api/config')
    return d

def serial_thread():
    try:
        import serial
        s = serial.Serial('/dev/ttyUSB0', 115200, timeout=1); s.dtr = True; s.rts = True
        buf = b''
        while not stop_serial:
            buf += s.read(4096)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                serial_lines.append(line.decode('utf-8', 'replace').rstrip())
        s.close()
    except Exception as e:
        serial_lines.append('SERIAL THREAD ERROR ' + repr(e))

def wait_for(pred, timeout=60, every=3):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if pred(): return True
        except Exception: pass
        time.sleep(every)
    return False

def now_hhmm(delta_min=0):
    return (datetime.datetime.now() + datetime.timedelta(minutes=delta_min)).strftime('%H:%M')

def today_dow():
    return (datetime.datetime.now().weekday() + 1) % 7  # Sunday=0

t = threading.Thread(target=serial_thread, daemon=True); t.start()
time.sleep(1)

backup = config()
assert backup, 'device not reachable'
json.dump(backup, open(os.path.join(S, 'test_backup_config.json'), 'w'))
base = copy.deepcopy(backup)
base['device']['invert_colors'] = False
s0 = state(); uptime0 = s0.get('uptime', 0); heap0 = s0.get('heap', 0)
print('== start: uptime', uptime0, 'heap', heap0, 'stops', [x['key'] for x in backup['stops']])

# ---------- A. endpoints ----------
code, body = get('/api/state'); check('A state 200 json', code == 200 and body.startswith(b'{'))
code, d = get_json('/api/config'); check('A config has all sections', code == 200 and all(k in d for k in ('device', 'stops', 'alerts', 'weather', 'due', 'profiles', 'bike')), d and list(d.keys()))
code, d = get_json('/api/debug/ui'); check('A debug/ui', code == 200 and d.get('page') in ('main', 'night', 'stats', 'device'), d)
code, d = get_json('/api/stats?stop=%s&days=30' % backup['stops'][0]['key']); check('A stats', code == 200 and isinstance(d, dict), (code, str(d)[:80]))
code, d = get_json('/api/log/index'); check('A log index', code == 200 and isinstance(d, list), (code, d))
if isinstance(d, list) and d:
    code, body = get('/api/log/' + d[0]['file'], pin=True); check('A log download', code == 200 and body.startswith(b'ts,'), (code, body[:40]))
    code, _ = get('/api/log/' + d[0]['file']); check('A log download needs the PIN', code == 401, code)
code, d = get_json('/api/rail/stations'); check('A rail stations', code == 200 and isinstance(d, list) and len(d) > 50, (code, type(d)))
code, body = get('/api/proxy/stops?route=17', 40); check('A proxy stops 17', code == 200 and len(body) > 10000 and b'21332' in body, (code, len(body)))
code, body = get('/api/proxy/schedule?stop_id=21332', 40); check('A proxy schedule', code == 200 and b'"17"' in body, (code, body[:60]))
r = curl(['-D', '-', '-o', '/dev/null', B + '/app.js']); h = r.stdout.decode(); check('A app.js gzip+etag', 'HTTP/1.1 200' in h and 'Content-Encoding: gzip' in h and 'ETag' in h, h[:200])
etag = re.search(r'ETag: (\S+)', h); etag = etag.group(1).strip() if etag else ''
r = curl(['-o', '/dev/null', '-w', '%{http_code}', '-H', 'If-None-Match: ' + etag, B + '/app.js']); check('A app.js 304 on ETag', r.stdout == b'304', r.stdout)
for p in ('/', '/app.css', '/favicon.svg'):
    code, body = get(p); check('A asset ' + p, code == 200 and len(body) > 100, code)
code, body = get('/nope'); check('A 404', code == 404, code)
code, body = get('/api/stats?stop=nope&days=30'); check('A stats unknown stop no crash', code in (200, 400, 404), code)
code, body = get_json('/api/stats/overview?days=7'); check('A stats overview', code == 200 and isinstance(body, dict) and 'stops' in body and 'bikes' in body and body.get('days') == 7, (code, str(body)[:120]))
code, body = get_json('/api/stats?stop=' + base['stops'][0]['key'] + '&days=7'); check('A stats v2 fields', code == 200 and 'crowding' in body and len(body.get('wait_by_hour', [])) == 24 and len(body['crowding'].get('by_hour', [])) == 24, (code, list(body)[:8] if isinstance(body, dict) else body))
code, raw = get('/api/log/index'); check('A log index', code == 200, code)
try:
    files = json.loads(raw) if raw else []
    newest = sorted(f['file'] for f in files)[-1] if files else None
except Exception:
    newest = None
if newest:
    r = curl(PINH + [B + '/api/log/' + newest], 120); lines = [l for l in r.stdout.decode(errors='replace').split('\n') if l.strip()]
    v2 = [l for l in lines[-40:] if l.count(',') == 20]
    check('A log rows are v2 (21 columns)', len(v2) > 0, (newest, len(lines), lines[-1][:80] if lines else ''))
    check('A log export header is schema v3 (temp_c)', lines and lines[0].startswith('ts,event,') and ',temp_c,' in lines[0], lines[0][:120] if lines else '')
    check('A log export rows all have 21 columns', all(l.count(',') == 20 for l in lines[1:]), [l[:60] for l in lines[1:] if l.count(',') != 20][:3])
    check('A log has bike rows', any(',bike,indego-' in l for l in lines) or not base['bike'].get('enabled'), newest)

# ---------- B. config round-trips ----------
def roundtrip(name, mutate, expect=None, wait=2.5):
    cfg = copy.deepcopy(base); mutate(cfg)
    code = put_cfg(cfg); time.sleep(wait)
    got = config()
    ok = code == 200 and got is not None and (expect(got) if expect else True)
    check('B ' + name, ok, (code, None if got is None else str(got.get('device', {}))[:160]))
    return got

roundtrip('name', lambda c: c['device'].update(name='display-test'), lambda g: g['device']['name'] == 'display-test')
roundtrip('poll_seconds 20', lambda c: c['device'].update(poll_seconds=20), lambda g: g['device']['poll_seconds'] == 20)
roundtrip('brightness 40', lambda c: c['device'].update(brightness=40), lambda g: g['device']['brightness'] == 40 and ui().get('brightness') == 40)
for rot, w, h in ((90, 480, 320), (180, 320, 480), (270, 480, 320), (0, 320, 480)):
    roundtrip('rotation %d' % rot, lambda c, rot=rot: c['device'].update(rotation=rot), lambda g, w=w, h=h: g['device']['rotation'] == rot and ui().get('hor_res') == w and ui().get('ver_res') == h)
roundtrip('theme dark', lambda c: c['device'].update(theme='dark'), lambda g: g['device']['theme'] == 'dark')
roundtrip('theme light', lambda c: c['device'].update(theme='light'), lambda g: g['device']['theme'] == 'light')
roundtrip('invert on', lambda c: c['device'].update(invert_colors=True), lambda g: g['device']['invert_colors'] is True)
roundtrip('invert off', lambda c: c['device'].update(invert_colors=False), lambda g: g['device']['invert_colors'] is False)
roundtrip('ticker off', lambda c: c['device'].update(ticker_show='off'), lambda g: g['device']['ticker_show'] == 'off' and ui().get('ticker') == '')
roundtrip('ticker alerts only', lambda c: c['device'].update(ticker_show='alerts'), lambda g: g['device']['ticker_show'] == 'alerts' and 'detour' not in ui().get('ticker', 'detour') and ui().get('ticker') != '')
roundtrip('ticker detours only', lambda c: c['device'].update(ticker_show='detours'), lambda g: g['device']['ticker_show'] == 'detours' and 'detour' in ui().get('ticker', '') and 'New Bus Network' not in ui().get('ticker', ''))
roundtrip('ticker both', lambda c: c['device'].update(ticker_show='both'), lambda g: 'detour' in ui().get('ticker', '') and ': ' in ui().get('ticker', ''))
roundtrip('ticker 1 line, speed 200', lambda c: c['device'].update(ticker_lines=1, ticker_speed=200), lambda g: g['device']['ticker_lines'] == 1 and g['device']['ticker_speed'] == 200)
roundtrip('ticker 8 lines, speed 5', lambda c: c['device'].update(ticker_lines=8, ticker_speed=5), lambda g: g['device']['ticker_lines'] == 8 and g['device']['ticker_speed'] == 5)
roundtrip('header all off', lambda c: c['device'].update(header={'name': False, 'clock': False, 'weather': False, 'wifi': False, 'updated': False}), lambda g: not any(g['device']['header'].values()))
roundtrip('header all on', lambda c: c['device'].update(header={'name': True, 'clock': True, 'weather': True, 'wifi': True, 'updated': True}), lambda g: all(g['device']['header'].values()))
roundtrip('large text on', lambda c: c['device'].update(large_text=True), lambda g: g['device']['large_text'] is True)
roundtrip('large text off', lambda c: c['device'].update(large_text=False), lambda g: g['device']['large_text'] is False)
roundtrip('crowding icons/crowd', lambda c: c['device'].update(crowding='icons', crowding_icons='crowd'), lambda g: g['device']['crowding'] == 'icons' and g['device']['crowding_icons'] == 'crowd')
roundtrip('crowding both/seats', lambda c: c['device'].update(crowding='both', crowding_icons='seats'), lambda g: g['device']['crowding'] == 'both' and g['device']['crowding_icons'] == 'seats')
roundtrip('crowding off', lambda c: c['device'].update(crowding='off'), lambda g: g['device']['crowding'] == 'off')
def _legacy_crowding(c):
    c['device'].pop('crowding', None); c['device']['show_crowding'] = False
roundtrip('crowding legacy show_crowding=false -> off', _legacy_crowding, lambda g: g['device']['crowding'] == 'off' and 'show_crowding' not in g['device'])
roundtrip('crowding words', lambda c: c['device'].update(crowding='words'), lambda g: g['device']['crowding'] == 'words')
def _icons_drawn():
    # every visible row with an icons string must lay out wider than 0 (the 2026-09-14 zero-width bug)
    rows = [r for r in ui().get('rows', '').split('\n') if r and not r.startswith('bike|')]
    with_icons = [r for r in rows if 'icons h=0' in r]
    return all(' w=0 ' not in r.split('|')[3] + ' ' for r in with_icons), (len(rows), len(with_icons))
roundtrip('crowding icons lay out (rows debug)', lambda c: c['device'].update(crowding='icons', crowding_icons='seats'), lambda g: g['device']['crowding'] == 'icons' and wait_for(lambda: _icons_drawn()[0], 10, 2))
roundtrip('bike style words', lambda c: c['bike'].update(style='words'), lambda g: g['bike']['style'] == 'words')
roundtrip('bike style icons', lambda c: c['bike'].update(style='icons'), lambda g: g['bike']['style'] == 'icons')
roundtrip('night off', lambda c: c['device']['night'].update(enabled=False, after_min=240), lambda g: g['device']['night'] == {'enabled': False, 'after_min': 240})
roundtrip('title styles', lambda c: (c['stops'][0].update(title_style='label'), c['stops'][1].update(title_style='custom', title_text='Uptown bus')), lambda g: g['stops'][0]['title_style'] == 'label' and g['stops'][1]['title_text'] == 'Uptown bus')
roundtrip('title route_dest_stop', lambda c: c['stops'][0].update(title_style='route_dest_stop'), lambda g: g['stops'][0]['title_style'] == 'route_dest_stop')
roundtrip('weather off', lambda c: c['weather'].update(enabled=False), lambda g: g['weather']['enabled'] is False and wait_for(lambda: ui().get('header_weather') == '', 10, 1) and state().get('weather', {}).get('enabled') is False)
roundtrip('weather celsius', lambda c: c['weather'].update(enabled=True, units='c', per_stop=False), lambda g: g['weather']['units'] == 'c', wait=8)
okc = wait_for(lambda: state().get('weather', {}).get('units') == 'c' and 0 < state()['weather']['main']['temp'] < 45, 60, 4)
sw = state().get('weather', {}); check('B state weather units c and temp plausible (within a poll)', okc, sw)
roundtrip('weather fahrenheit', lambda c: c['weather'].update(units='f', per_stop=True), lambda g: g['weather']['units'] == 'f', wait=8)
roundtrip('due off', lambda c: c['due'].update(enabled=False), lambda g: g['due']['enabled'] is False and ui().get('due_active') is False)
roundtrip('logging off', lambda c: c['device'].update(logging=False), lambda g: g['device']['logging'] is False)
roundtrip('alerts off', lambda c: c.update(alerts=False), lambda g: g['alerts'] is False, wait=8)
check('B alerts off -> ticker empty', wait_for(lambda: ui().get('ticker') == '' and state().get('alerts') == [], 10, 1), (ui().get('ticker', '')[:40], len(state().get('alerts', []))))
roundtrip('profiles x4', lambda c: c.update(profiles=[{'name': 'P%d' % i, 'days': [0, 6], 'start': '01:00', 'end': '02:00', 'stops': [c['stops'][0]['key']]} for i in range(4)]), lambda g: len(g['profiles']) == 4 and g['profiles'][3]['days'] == [0, 6])
roundtrip('bike 3 stations', lambda c: c.update(bike={'enabled': True, 'stations': [{'id': 3468, 'name': 'Snyder & Dorrance'}, {'id': 3361, 'name': '18th & Fernon'}, {'id': 3053, 'name': 'Point Breeze & Tasker'}]}), lambda g: len(g['bike']['stations']) == 3, wait=25)
ok3 = wait_for(lambda: len(state().get('bike', {}).get('stations', [])) == 3 and all(x['bikes'] >= 0 for x in state()['bike']['stations']), 90, 5)
sb = state().get('bike', {}); check('B state bike 3 stations with counts', ok3, sb)
roundtrip('bike off', lambda c: c.update(bike={'enabled': False, 'stations': []}), lambda g: g['bike']['enabled'] is False, wait=4)
check('B state bike disabled', wait_for(lambda: state().get('bike', {}).get('enabled') is False and state()['bike']['stations'] == [], 10, 1), state().get('bike'))
put_cfg(copy.deepcopy(base)); time.sleep(3)
check('B restore base', config()['device']['name'] == base['device']['name'])

# ---------- C. validation ----------
def invalid(name, mutate, path):
    cfg = copy.deepcopy(base); mutate(cfg)
    code, body = put_cfg_body(cfg)
    ok = code == 400 and body and body.get('path') == path
    check('C ' + name, ok, (code, body))
invalid('title_style', lambda c: c['stops'][0].update(title_style='fancy'), 'stops[0].title_style')
invalid('title_text too long', lambda c: c['stops'][0].update(title_text='x' * 41), 'stops[0].title_text')
invalid('alt_of self', lambda c: c['stops'][0].update(alt_of=c['stops'][0]['key']), 'stops[0].alt_of')
invalid('alt_of unknown', lambda c: c['stops'][0].update(alt_of='nope'), 'stops[0].alt_of')
invalid('alt_after_min 4', lambda c: c['stops'][0].update(alt_of=c['stops'][1]['key'], alt_after_min=4), 'stops[0].alt_after_min')
invalid('rotation 45', lambda c: c['device'].update(rotation=45), 'device.rotation')
invalid('theme blue', lambda c: c['device'].update(theme='blue'), 'device.theme')
invalid('crowding bad mode', lambda c: c['device'].update(crowding='sometimes'), 'device.crowding')
invalid('crowding bad icons', lambda c: c['device'].update(crowding_icons='cats'), 'device.crowding_icons')
invalid('bike style bad', lambda c: c['bike'].update(style='emoji'), 'bike.style')
invalid('ticker_show sometimes', lambda c: c['device'].update(ticker_show='sometimes'), 'device.ticker_show')
invalid('ticker_lines 9', lambda c: c['device'].update(ticker_lines=9), 'device.ticker_lines')
invalid('ticker_speed 4', lambda c: c['device'].update(ticker_speed=4), 'device.ticker_speed')
invalid('quiet start 25:00', lambda c: c['device']['quiet'].update(start='25:00'), 'device.quiet.start')
invalid('quiet brightness 51', lambda c: c['device']['quiet'].update(brightness=51), 'device.quiet.brightness')
invalid('quiet wake 4', lambda c: c['device']['quiet'].update(wake_seconds=4), 'device.quiet.wake_seconds')
invalid('night after_min 14', lambda c: c['device']['night'].update(after_min=14), 'device.night.after_min')
invalid('due minutes 16', lambda c: c['due'].update(minutes=16), 'due.minutes')
invalid('profiles x5', lambda c: c.update(profiles=[{'name': 'P', 'days': [1], 'start': '01:00', 'end': '02:00', 'stops': [c['stops'][0]['key']]}] * 5), 'profiles')
invalid('profile bad stop', lambda c: c.update(profiles=[{'name': 'P', 'days': [1], 'start': '01:00', 'end': '02:00', 'stops': ['nope']}]), 'profiles[0].stops')
invalid('profile bad time', lambda c: c.update(profiles=[{'name': 'P', 'days': [1], 'start': '1:00', 'end': '02:00', 'stops': [c['stops'][0]['key']]}]), 'profiles[0].start')
invalid('profile empty name', lambda c: c.update(profiles=[{'name': '', 'days': [1], 'start': '01:00', 'end': '02:00', 'stops': [c['stops'][0]['key']]}]), 'profiles[0].name')
invalid('bike 4 stations', lambda c: c.update(bike={'enabled': True, 'stations': [{'id': i, 'name': 's'} for i in range(1, 5)]}), 'bike.stations')
invalid('bike id 0', lambda c: c.update(bike={'enabled': True, 'stations': [{'id': 0, 'name': 's'}]}), 'bike.stations[0].id')
invalid('weather units k', lambda c: c['weather'].update(units='k'), 'weather.units')
invalid('poll_seconds 4', lambda c: c['device'].update(poll_seconds=4), 'device.poll_seconds')
invalid('brightness 101', lambda c: c['device'].update(brightness=101), 'device.brightness')
invalid('empty name', lambda c: c['device'].update(name=''), 'device.name')
invalid('show 5', lambda c: c['stops'][0].update(show=5), 'stops[0].show')
invalid('duplicate key', lambda c: c['stops'][1].update(key=c['stops'][0]['key']), 'stops[1].key')
invalid('9 stops', lambda c: c.update(stops=[dict(c['stops'][0], key='k%d' % i) for i in range(9)]), 'stops')
invalid('lat 91', lambda c: c['stops'][0].update(lat=91), 'stops[0].lat')
r = curl(['-w', '%{http_code}', '-o', '/dev/null', '-X', 'PUT', '-H', 'Content-Type: application/json', '--data-binary', '{not json', B + '/api/config']); check('C malformed json rejected', r.stdout in (b'400', b'415'), r.stdout)
check('C config unchanged after invalid PUTs', config()['device']['name'] == base['device']['name'])

# ---------- C2. validation added by the hardening pass (review F07/F30) ----------
invalid('brightness 256 (must not wrap to 0)', lambda c: c['device'].update(brightness=256), 'device.brightness')
invalid('poll_seconds 65566 (must not wrap to 30)', lambda c: c['device'].update(poll_seconds=65566), 'device.poll_seconds')
invalid('duplicate profile stop keys', lambda c: c.update(profiles=[{'name': 'Dup', 'days': [1], 'start': '01:00', 'end': '02:00',
        'stops': [c['stops'][0]['key'], c['stops'][0]['key']]}]), 'profiles[0].stops')
invalid('device name with a space', lambda c: c['device'].update(name='transit display'), 'device.name')
invalid('control character in a label', lambda c: c['stops'][0].update(label='17\nSouthbound'), 'stops[0].label')

def _rail_stop(line):
    return {'key': 'rail-30th-N', 'mode': 'rail', 'station': '30th Street Station', 'direction': 'N',
            'line': line, 'label': 'Regional Rail North', 'show': 2}

cfg = copy.deepcopy(base); cfg['stops'] = cfg['stops'] + [_rail_stop('Paoli/Thorndale')]
code = put_cfg(cfg); time.sleep(2.5)
got = config()
rail = [x for x in (got or {}).get('stops', []) if x['key'] == 'rail-30th-N']
check('C2 rail display name normalised to a line code', code == 200 and rail and rail[0].get('line') == 'PAO', (code, rail))
cfg['stops'][-1]['line'] = 'Not A Line'
code, body = put_cfg_body(cfg)
check('C2 unknown rail line rejected', code == 400 and body and body.get('path') == 'stops[2].route', (code, body))
put_cfg(copy.deepcopy(base)); time.sleep(2.5)

# ---------- C3. auth and cross-origin hardening (review F01/F05) ----------
code, body = put_cfg_nopin(copy.deepcopy(base))
check('C3 PUT /api/config without a PIN is 401', code == 401 and body and body.get('error') == 'pin required', (code, body))
r = curl(['-o', '/dev/null', '-w', '%{http_code}', '-X', 'POST', B + '/api/reboot'])
check('C3 POST /api/reboot without a PIN is 401', r.stdout == b'401', r.stdout)
code, d = get_json('/api/state')
check('C3 state advertises pin_required and board', code == 200 and d.get('auth', {}).get('pin_required') is True and isinstance(d.get('board'), str) and d['board'], (code, d.get('auth'), d.get('board')))
check('C3 state reports config_recovered', 'config_recovered' in (d or {}), list(d or {})[:12])
code, _ = get_json('/api/config')
check('C3 GET /api/config stays open', code == 200, code)

# Five wrong PINs in a row lock every protected route for 30 s, then it recovers on its own.
bad = '000000' if PIN != '000000' else '999999'
codes = [put_cfg(copy.deepcopy(base), pin=bad) for _ in range(4)]
check('C3 a wrong PIN is 401', codes == [401, 401, 401, 401], codes)
check('C3 the 5th wrong PIN locks out (429)', put_cfg(copy.deepcopy(base), pin=bad) == 429)
locked = put_cfg(copy.deepcopy(base))
check('C3 locked out even with the right PIN', locked == 429, locked)
r = curl(['-w', '\n%{http_code}', '-X', 'PUT', '-H', 'X-Pin: ' + PIN, '-H', 'Content-Type: application/json',
          '--data-binary', json.dumps(base), B + '/api/config'])
lbody, _, lcode = r.stdout.rpartition(b'\n')
try: lj = json.loads(lbody)
except Exception: lj = {}
check('C3 429 body carries retry_s', lcode == b'429' and isinstance(lj.get('retry_s'), int) and 0 < lj['retry_s'] <= 30, (lcode, lj))
time.sleep(32)
check('C3 recovers after the lockout expires', put_cfg(copy.deepcopy(base)) == 200)

# Host-header check: a rebinding attacker's own domain must not be served (421).
r = curl(['-o', '/dev/null', '-w', '%{http_code}', '-H', 'Host: evil.example.com', B + '/api/state'])
check('C3 wrong Host is 421', r.stdout == b'421', r.stdout)
r = curl(['-o', '/dev/null', '-w', '%{http_code}', '-H', 'Host: ' + B.replace('http://', '') + ':80', B + '/api/state'])
check('C3 own IP with a port is accepted', r.stdout == b'200', r.stdout)
r = curl(['-D', '-', '-o', '/dev/null', B + '/']); h = r.stdout.decode()
check('C3 index sends frame + nosniff headers', 'X-Frame-Options: DENY' in h and "frame-ancestors 'none'" in h and 'X-Content-Type-Options: nosniff' in h, h[:300])
time.sleep(2)

# ---------- D. screen logic ----------
k0, k1 = base['stops'][0]['key'], base['stops'][1]['key']
# profiles
cfg = copy.deepcopy(base); cfg['profiles'] = [{'name': 'Test window', 'days': [today_dow()], 'start': now_hhmm(-5), 'end': now_hhmm(30), 'stops': [k1]}]
put_cfg(cfg); time.sleep(3); d = ui()
check('D profile active narrows shown stops', d.get('active_profile') == 'Test window' and d.get('shown_stops') == [k1], d)
check('D state reports active profile', state().get('active_profile') == 'Test window')
cfg['profiles'][0]['days'] = [(today_dow() + 3) % 7]; put_cfg(cfg); time.sleep(3); d = ui()
check('D profile inactive on other day', d.get('active_profile') == '' and len(d.get('shown_stops', [])) == len(base['stops']), d)
# alternatives: k1 alternative to k0 with 60 min -> hidden while k0 has a bus within 60 min
cfg = copy.deepcopy(base); cfg['stops'][1].update(alt_of=k0, alt_after_min=60); put_cfg(cfg); time.sleep(3); d = ui()
check('D alternative hidden while primary is near', k1 in d.get('hidden_panels', []), d)
# alternative to a stop with no data -> shown
cfg = copy.deepcopy(base); tmp = dict(cfg['stops'][0]); tmp.update(key='tmp-99999', stop_id='99999', label='Temp no-service', stop_name='nowhere', lat=0, lng=0, alt_of='', alt_after_min=15)
cfg['stops'].append(tmp); cfg['stops'][1].update(alt_of='tmp-99999', alt_after_min=5); put_cfg(cfg)
present = wait_for(lambda: any(x['key'] == 'tmp-99999' for x in state().get('stops', [])), 90, 4)
time.sleep(2); d = ui()
check('D alternative shown when primary has no data', k1 not in d.get('hidden_panels', []), d)
st = state(); tmp_snap = [x for x in st.get('stops', []) if x['key'] == 'tmp-99999']
check('D temp stop present with no arrivals (after the next poll)', present and tmp_snap and tmp_snap[0].get('arrivals') == [], (present, tmp_snap))
# night page: profile showing only the no-service stop
cfg['profiles'] = [{'name': 'Night test', 'days': [today_dow()], 'start': now_hhmm(-5), 'end': now_hhmm(30), 'stops': ['tmp-99999']}]
cfg['stops'][1].update(alt_of='', alt_after_min=15); cfg['device']['night'] = {'enabled': True, 'after_min': 60}
put_cfg(cfg)
check('D night page when nothing is due', wait_for(lambda: ui().get('page') == 'night', 90, 3), ui())
check('D tap from night goes to stats', post('/api/debug/tap') == 200 and wait_for(lambda: ui().get('page') == 'stats', 10, 1), ui().get('page'))
check('D tap to device page', post('/api/debug/tap') == 200 and (time.sleep(2) or ui().get('page') == 'device'))
check('D tap back to night', post('/api/debug/tap') == 200 and (time.sleep(2) or ui().get('page') == 'night'))
cfg['device']['night']['enabled'] = False; put_cfg(cfg); time.sleep(3)
check('D night disabled -> main page', ui().get('page') == 'main', ui())
put_cfg(copy.deepcopy(base)); time.sleep(3)
check('D back to base: main, all stops', ui().get('page') == 'main' and len(ui().get('shown_stops', [])) == len(base['stops']), ui())
# due + chime
cfg = copy.deepcopy(base); cfg['due'] = {'enabled': True, 'minutes': 15, 'led': True, 'screen': True, 'chime': True}
chimes0 = ui().get('chimes', 0); put_cfg(cfg); time.sleep(4); d = ui()
first_eta = min([a['eta_s'] for x in state().get('stops', []) for a in x.get('arrivals', [])] or [9999])
check('D due active with 15 min window', d.get('due_active') is True or first_eta > 900, (d.get('due_active'), first_eta))
check('D chime played for a live due trip', d.get('chimes', 0) > chimes0 or first_eta > 900, (chimes0, d.get('chimes'), first_eta))
put_cfg(copy.deepcopy(base)); time.sleep(3)
# quiet hours + wake by tap
cfg = copy.deepcopy(base); cfg['device']['quiet'] = {'enabled': True, 'start': now_hhmm(-5), 'end': now_hhmm(30), 'brightness': 10, 'wake_seconds': 5}
put_cfg(cfg); time.sleep(3); d = ui()
check('D quiet hours dim', d.get('dimmed') is True and d.get('brightness') == 10, d)
page_before = d.get('page'); post('/api/debug/tap'); time.sleep(2); d = ui()
check('D tap wakes without changing page', d.get('dimmed') is False and d.get('brightness') == base['device']['brightness'] and d.get('page') == page_before, d)
time.sleep(6); d = ui()
check('D dims again after wake_seconds', d.get('dimmed') is True and d.get('brightness') == 10, d)
cfg['device']['quiet']['start'] = now_hhmm(60); cfg['device']['quiet']['end'] = now_hhmm(90); put_cfg(cfg); time.sleep(3); d = ui()
check('D outside quiet window: normal brightness', d.get('dimmed') is False and d.get('brightness') == base['device']['brightness'], d)
put_cfg(copy.deepcopy(base)); time.sleep(3)
# page cycle from main
check('D tap main->stats', post('/api/debug/tap') == 200 and (time.sleep(2) or ui().get('page') == 'stats'), ui().get('page'))
check('D tap stats->device', post('/api/debug/tap') == 200 and (time.sleep(2) or ui().get('page') == 'device'), ui().get('page'))
check('D tap device->main', post('/api/debug/tap') == 200 and (time.sleep(2) or ui().get('page') == 'main'), ui().get('page'))
# weather notes + crowding presence in state
st = state(); seats = [a.get('seats') for x in st['stops'] for a in x['arrivals'] if a.get('status') == 'live']
check('D live rows carry seat data', any(seats) or not seats, seats[:3])
check('D weather_note field present on stops', all('weather_note' in x for x in st['stops']))
# Review F13/F15/F26/F29 fields (DESIGN.md SS7): per-stop health, data age, matched schedule trip,
# SD write health and weather staleness must all be present and well-formed.
check('D stop health is one of the four tokens', all(x.get('health') in ('live', 'schedule_only', 'stale', 'unavailable') for x in st['stops']), [(x['key'], x.get('health')) for x in st['stops']])
check('D stop source_age_s present', all(isinstance(x.get('source_age_s'), int) for x in st['stops']), [x.get('source_age_s') for x in st['stops']])
check('D arrivals carry sched_trip', all('sched_trip' in a for x in st['stops'] for a in x['arrivals']))
check('D live stops are not stale right after a poll', all(x.get('health') != 'stale' for x in st['stops'] if x.get('ok')), [(x['key'], x.get('health'), x.get('source_age_s')) for x in st['stops']])
sdj = st.get('sd', {})
check('D sd write health fields', isinstance(sdj.get('dropped_rows'), int) and isinstance(sdj.get('write_ok'), bool) and 'error' in sdj, sdj)
check('D sd dropped no rows', sdj.get('dropped_rows') == 0 or not sdj.get('mounted'), sdj)
wxj = st.get('weather', {})
check('D weather age/stale fields', isinstance(wxj.get('age_s'), int) and isinstance(wxj.get('stale'), bool), wxj.get('age_s'))
check('D state reports board and auth', st.get('board') == 'cyd-3248S035R' and st.get('auth', {}).get('pin_required') is True and 'config_recovered' in st, (st.get('board'), st.get('auth')))
d = ui(); check('D LVGL pool has headroom', d.get('lv_free', 0) > 3000, (d.get('lv_used'), d.get('lv_free'), d.get('lv_max_used')))

# ---------- E. concurrency ----------
def hit(u):
    r = curl(['-o', '/dev/null', '-w', '%{http_code} %{size_download}', B + u], 40); return r.stdout.decode()
u_before = state().get('uptime', 0)
outcomes = []
for rnd in range(3):
    with concurrent.futures.ThreadPoolExecutor(7) as ex:
        outcomes.append(list(ex.map(hit, ['/api/state'] * 4 + ['/api/proxy/stops?route=17', '/api/stats?stop=%s&days=30' % k0, '/app.js'])))
    time.sleep(5)
u_after = state().get('uptime', 0)
check('E no reboot under 3x7 concurrent requests', u_after > u_before, (u_before, u_after, outcomes))
empties = sum(1 for r in outcomes for o in r if o.startswith('200 0'))
check('E at most 2 empty responses per round (known limit, firmware/README.md)', empties <= 6, (empties, outcomes))

# ---------- F. reboot ----------
check('F reboot endpoint', post('/api/reboot') == 200)
time.sleep(6)
deadline = time.time() + 90
while time.time() < deadline:
    code, d = get_json('/api/state')
    if code == 200 and d: break
    time.sleep(3)
check('F back after reboot', code == 200 and d and d.get('uptime', 999) < 90, (code, d and d.get('uptime')))
check('F config intact after reboot', config() and config()['device']['name'] == base['device']['name'])

# ---------- G. OTA with the running image ----------
fw = '/home/hoid/Desktop/philly-transit-display/firmware/.pio/build/cyd-3248S035R/firmware.bin'
if os.path.exists(fw):
    ver_before = state().get('firmware_version')
    # The OTA handler refuses uploads below 60 KB free heap, and right after section F's reboot the
    # first poll (Indego's 400 KB stream included) is still running: wait for it and for heap to settle.
    wait_for(lambda: state().get('last_poll', {}).get('ok') and state().get('heap', 0) > 70000, 120, 5)
    # An upload with no file at all, and one for a different board, must both be refused before
    # anything is written (review F08).
    r = curl(PINH + ['-w', '\n%{http_code}', '-X', 'POST', B + '/api/ota'], 30)
    nbody, _, ncode = r.stdout.rpartition(b'\n')
    check('G OTA with no file is 400', ncode == b'400' and b'no firmware file' in nbody, (ncode, nbody[:120]))
    fake = os.path.join(S, 'test_wrong_board.bin')
    with open(fake, 'wb') as fh:
        fh.write(b'\xe9\x04\x02\x20' + bytes(range(256)) * 16)  # ESP32 image magic, then filler; no board marker
    r = curl(PINH + ['-w', '\n%{http_code}', '-F', 'firmware=@' + fake, B + '/api/ota'], 60)
    wbody, _, wcode = r.stdout.rpartition(b'\n')
    check('G OTA rejects an image for a different board', wcode == b'400' and b'different board' in wbody, (wcode, wbody[:160]))
    os.remove(fake)
    check('G device still up after the rejected uploads', state().get('uptime', 0) > 0)
    r = curl(PINH + ['-w', '\n%{http_code}', '-F', 'firmware=@' + fw, B + '/api/ota'], 180)
    body, _, code = r.stdout.rpartition(b'\n')
    check('G OTA upload accepted', code in (b'200', b'202'), (code, body[:120]))
    time.sleep(8)
    back = wait_for(lambda: state().get('uptime', 999) < 120, 120, 4)
    check('G device back after OTA', back, state().get('uptime'))
    check('G firmware version unchanged after re-flash', state().get('firmware_version') == ver_before, (ver_before, state().get('firmware_version')))
    check('G config intact after OTA', config() and config()['device']['name'] == base['device']['name'])
    time.sleep(30)

# ---------- restore ----------
put_cfg(base); time.sleep(3)
final = config()
check('Z restored owner config (invert off)', final and [x['key'] for x in final['stops']] == [x['key'] for x in backup['stops']] and final['device']['invert_colors'] is False)
stop_serial = True; time.sleep(1.5)
crashes = [l for l in serial_lines if re.search(r'Guru|abort\(\)|Backtrace|rst:0x', l)]
warns = sum(1 for l in serial_lines if '[Warn]' in l)
errs = [l for l in serial_lines if re.search(r'\bE \(|error', l, re.I)][:5]
check('Z serial: no crash lines except the requested reboot and the OTA reboot', len([l for l in crashes if 'rst:0x' in l]) <= 2 and not [l for l in crashes if 'Guru' in l or 'Backtrace' in l or 'abort' in l], crashes[:5])
check('Z serial: no LVGL warnings', warns == 0, warns)
print('== serial lines captured', len(serial_lines), 'errors sample', errs)
passed = sum(1 for r in results if r[1]); print('\n== %d/%d checks passed' % (passed, len(results)))
for name, ok, detail in results:
    if not ok: print('   FAILED:', name, '--', str(detail)[:300])
json.dump([(n, ok, str(d)) for n, ok, d in results], open(os.path.join(S, 'exhaustive_results.json'), 'w'))

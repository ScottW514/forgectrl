#!/usr/bin/env python3
"""Panel development server: live reload for the control panel.

Serves the panel from src/ui/ (index.html, panel.css, panel.js) as plain
files - so the browser's devtools see real file names and line numbers -
and reloads the open tab through an injected watcher as soon as anything
in that directory changes, while every API request is either

  * proxied to a real machine (GF_HOST / GF_TOKEN, from the environment
    or the .env file at the repo root; see .env.example), so the panel
    shows live data and its actions reach the hardware exactly as they
    would from the machine's own port 8080; or
  * answered by the built-in mock backend (--mock, or automatically when
    no GF_HOST is configured), which exercises the JavaScript without a
    machine and mirrors the daemon's token check on state-changing
    endpoints.

Usage:
    python3 tools/devserver.py [--port 8081] [--bind 127.0.0.1]
                               [--host ADDR[:PORT]] [--token HEX]
                               [--env FILE] [--mock] [--bundle] [-v]
    python3 tools/devserver.py --dump    # the bundled page to stdout

Then browse http://127.0.0.1:8081 and edit src/ui/. --bundle serves the
page the way the daemon does (CSS and JS inlined, one response), which is
also what --dump prints; the bundling mirrors src/ui/embed.cmake.

Requires only the Python 3 standard library.
"""
import argparse
import hashlib
import http.client
import io
import json
import os
import socket
import sys
import tarfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qsl

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
UI_DIR = os.path.join(ROOT, 'src', 'ui')
ENV_FILE = os.path.join(ROOT, '.env')

TOKEN_MARK = '__FFTOKEN__'          # in panel.js; the daemon substitutes it
CSS_TAG = '<link rel="stylesheet" href="panel.css" />'   # as embed.cmake
JS_TAG = '<script src="panel.js"></script>'

DEFAULT_PORT = 8081
DEVICE_PORT = 8080
MOCK_TOKEN = '0123456789abcdef0123456789abcdef'
WATCH_POLL_S = 0.25         # src/ui/ stat interval
WATCH_HOLD_S = 25.0         # long-poll ceiling per /__dev/watch call
PROXY_TIMEOUT_S = 30.0      # per-request upstream timeout (streams: none)
STREAM_PATHS = ('/cam/stream',)

HOP_BY_HOP = {
    'connection', 'keep-alive', 'proxy-authenticate',
    'proxy-authorization', 'te', 'trailers', 'transfer-encoding',
    'upgrade',
}
# The daemon's anti-rebinding / anti-CSRF layer wants an address-literal
# Host and no foreign Origin. The panel is same-origin to this server,
# so these browser-side headers describe the dev origin, not the
# machine's - they are dropped and Host is rewritten to the device.
DROP_REQ_HEADERS = HOP_BY_HOP | {
    'host', 'origin', 'referer', 'accept-encoding',
    'sec-fetch-site', 'sec-fetch-mode', 'sec-fetch-dest',
    'sec-fetch-user',
}
DROP_RESP_HEADERS = HOP_BY_HOP | {'content-length'}


# ------------------------------------------------------------ .env
def parse_env_file(path):
    """KEY=VALUE per line; '#' comments, optional 'export ', optional
    single/double quotes. Returns {} for a missing file."""
    out = {}
    try:
        with open(path, encoding='utf-8') as f:
            lines = f.read().splitlines()
    except OSError:
        return out
    for ln in lines:
        s = ln.strip()
        if not s or s.startswith('#') or '=' not in s:
            continue
        if s.startswith('export '):
            s = s[7:].lstrip()
        k, v = s.split('=', 1)
        k = k.strip()
        v = v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
            v = v[1:-1]
        elif ' #' in v:
            v = v[:v.index(' #')].rstrip()
        if k:
            out[k] = v
    return out


class Config:
    """Effective device settings: CLI > process environment > .env.
    The .env file is re-read whenever it changes, so switching machines
    (or fixing a token) does not need a restart."""

    def __init__(self, args):
        self.args = args
        self.env_path = args.env or ENV_FILE
        self._stamp = None
        self._file = {}
        self._lock = threading.Lock()
        self.refresh()

    def _stat(self):
        try:
            st = os.stat(self.env_path)
            return (st.st_mtime_ns, st.st_size)
        except OSError:
            return None

    def refresh(self):
        with self._lock:
            st = self._stat()
            if st != self._stamp:
                self._stamp = st
                self._file = parse_env_file(self.env_path)

    def get(self, key):
        self.refresh()
        return self._file.get(key, '')

    @property
    def mock(self):
        return bool(self.args.mock) or not self.host

    @property
    def host(self):
        h = self.args.host or os.environ.get('GF_HOST') or self.get('GF_HOST')
        return (h or '').strip()

    @property
    def token(self):
        t = (self.args.token or os.environ.get('GF_TOKEN') or
             self.get('GF_TOKEN'))
        return (t or '').strip()

    def upstream(self):
        """(ip, port, display) for the device; the daemon accepts only an
        address-literal Host, so a name is resolved here once per call
        and the literal is what goes on the wire."""
        h = self.host
        if '://' in h:                              # a pasted URL is fine
            h = h.split('://', 1)[1]
        h = h.split('/', 1)[0]
        port = DEVICE_PORT
        if h.startswith('['):                       # [v6]:port
            end = h.find(']')
            name = h[1:end]
            rest = h[end + 1:]
            if rest.startswith(':'):
                port = int(rest[1:])
        elif h.count(':') == 1:
            name, p = h.split(':')
            port = int(p)
        else:
            name = h
        try:
            ip = socket.getaddrinfo(name, port)[0][4][0]
        except (socket.gaierror, IndexError):
            ip = name
        if ':' in ip:
            ip = '[%s]' % ip                        # bracket a v6 literal
        return ip, port, '%s:%d' % (name if ':' not in name else
                                    '[%s]' % name, port)


# ------------------------------------------------------------ panel
RELOAD_JS = (
    "<script>(function(){var v=%(ver)s;"
    "function w(){var x=new XMLHttpRequest();"
    "x.open('GET','/__dev/watch?v='+encodeURIComponent(v),true);"
    "x.onreadystatechange=function(){if(x.readyState!==4)return;"
    "if(x.status===200){var j;try{j=JSON.parse(x.responseText)}catch(e){}"
    "if(j&&j.v!==v){location.reload();return}w()}"
    "else{setTimeout(w,1500)}};"
    "x.onerror=function(){setTimeout(w,1500)};x.send()}w()})()</script>"
)
BADGE_HTML = (
    "<div id='__devbadge' style='position:fixed;right:10px;bottom:10px;"
    "z-index:9999;background:%(bg)s;color:#fff;font:600 11px/1 "
    "system-ui,sans-serif;padding:6px 9px;border-radius:5px;opacity:.85;"
    "letter-spacing:.3px;pointer-events:none'>DEV &middot; %(label)s</div>"
)
CONTENT_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.svg': 'image/svg+xml',
    '.png': 'image/png',
    '.ico': 'image/x-icon',
}


def html_escape(s):
    return (str(s).replace('&', '&amp;').replace('<', '&lt;')
            .replace('>', '&gt;'))


def bundle(html, css, js):
    """One self-contained page - the same replacement embed.cmake does,
    so `--dump` / `--bundle` show exactly what the daemon serves."""
    for tag, rep in ((CSS_TAG, '<style>\n' + css + '</style>'),
                     (JS_TAG, '<script>\n' + js + '</script>')):
        if tag not in html:
            raise ValueError('index.html lacks the marker %s' % tag)
        html = html.replace(tag, rep, 1)
    return html


class Panel:
    """The files under src/ui/. A watcher thread stamps the directory and
    wakes the long-poll waiters when anything in it changes."""

    def __init__(self, ui_dir):
        self.dir = ui_dir
        self.cond = threading.Condition()
        self.version = self._stamp()
        t = threading.Thread(target=self._watch, daemon=True)
        t.start()

    def _stamp(self):
        parts = []
        try:
            names = sorted(os.listdir(self.dir))
        except OSError:
            return 'missing'
        for n in names:
            try:
                st = os.stat(os.path.join(self.dir, n))
            except OSError:
                continue
            parts.append('%s:%d:%d' % (n, st.st_mtime_ns, st.st_size))
        return hashlib.sha1('|'.join(parts).encode()).hexdigest()[:16]

    def _watch(self):
        while True:
            time.sleep(WATCH_POLL_S)
            v = self._stamp()
            if v != self.version:
                with self.cond:
                    self.version = v
                    self.cond.notify_all()

    def wait_change(self, seen, timeout):
        """Block until the version differs from `seen` or the timeout
        passes; returns the current version."""
        with self.cond:
            if self.version == seen:
                self.cond.wait(timeout)
            return self.version

    def path(self, name):
        """Absolute path of a file in src/ui/, or None if `name` is not a
        plain file there (no traversal, no dotfiles)."""
        if not name or '/' in name or '\\' in name or name.startswith('.'):
            return None
        p = os.path.join(self.dir, name)
        return p if os.path.isfile(p) else None

    def read(self, name):
        with open(os.path.join(self.dir, name), 'rb') as f:
            return f.read()

    def text(self, name):
        return self.read(name).decode('utf-8')

    def page(self, bundled):
        """index.html as text: the three files linked (dev default, real
        file names and line numbers in the browser) or inlined."""
        html = self.text('index.html')
        if bundled:
            html = bundle(html, self.text('panel.css'), self.text('panel.js'))
        return html

    def render(self, token, label, mock, bundled):
        page = self.page(bundled).replace(TOKEN_MARK, token or '', 1)
        badge = BADGE_HTML % {'bg': '#c7760a' if mock else '#3d854d',
                              'label': html_escape(label)}
        inject = badge + (RELOAD_JS % {'ver': json.dumps(self.version)})
        k = page.rfind('</body>')
        return page + inject if k < 0 else page[:k] + inject + page[k:]


# ------------------------------------------------------------- mock
MOCK_SVG = (
    "<svg xmlns='http://www.w3.org/2000/svg' width='640' height='360' "
    "viewBox='0 0 640 360'><rect width='640' height='360' fill='#3a3d46'/>"
    "<rect x='40' y='30' width='560' height='300' fill='none' "
    "stroke='#8b8f99' stroke-dasharray='8 6'/>"
    "<text x='320' y='170' fill='#d5d8de' font-family='system-ui,sans-serif' "
    "font-size='26' text-anchor='middle'>lid camera (mock)</text>"
    "<text x='320' y='205' fill='#8b8f99' font-family='system-ui,sans-serif' "
    "font-size='14' text-anchor='middle'>%s</text></svg>"
)


class Mock:
    """In-memory stand-in for the daemon: enough of every endpoint the
    panel touches for the JavaScript to run end to end. State-changing
    calls are token-checked like the real thing so a missing fx() shows
    up as the same 403 it would on the machine."""

    def __init__(self, token):
        self.token = token
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.settings = {
            'controller_mode': 'grbl', 'homing_mode': 'gfcloud',
            'gfcloud_home_x': '', 'gfcloud_home_y': '', 'gfcloud_home_z': '',
            'gfcloud_home_timeout_s': '', 'gf_serial': '',
            'gf_password_set': False, 'ui_units': '', 'wifi_country': '',
            'cool_flow_rise': '', 'cool_flow_heater_pct': '',
            'cool_flow_check_s': '', 'cool_recheck_s': '',
            'cool_confirm_max_s': '', 'cool_temp_max': '',
            'cool_temp_resume': '', 'cool_cooldown_s': '',
            'cool_cooldown_max_s': '', 'laser_button_timeout_s': '',
            'laser_disarm_s': '', 'rail_settle_s': '', 'lid_lamp_idle': '',
            'cloud_pause_backtrack_ticks': '', 'cloud_resume_lead_ticks': '', 'lid_policy': '',
            'syslog_server': '', 'syslog_port': '', 'syslog_proto': '',
            'version': '20260101000000 (mock)', 'machine_id': 'ABC-123',
        }
        for lg in ('forgectrl', 'grblhal', 'gfcloud', 'gfhome', 'kernel',
                   'system'):
            self.settings['log_%s_disk' % lg] = ''
            self.settings['log_%s_remote' % lg] = ''
        self.mode = 'grbl'
        self.status = {
            'state': 'idle', 'homed': True, 'diag': False,
            'pos': {'x': 12.34, 'y': -5.6, 'z': 0.0},
            'laser_locked': True,
            'laser': {'emission_samples': 0, 'pgood_samples': 0},
            'faults': 0, 'hv_current_raw': 0, 'lid_ir': [2, 2, 3, 2],
            'fans': {'air_assist': 1990, 'exhaust': 0, 'intake_1': 720,
                     'intake_2': 730},
            'coolant': {'down_c': 22.4, 'up_c': 22.3, 'pump': True,
                        'tec': False},
            'gfsvc': {'latest': '2.6.0', 'tested': '2.6.0'},
            'gates_off': [],
            'switches': {'lid': True, 'button': False, 'interlock_ok': True,
                         'head': False, 'hv_enable': False},
        }
        self.diag = {
            'running': False, 'tool': 'flow-calibrate', 'phase': 'done',
            'elapsed_s': 0, 'down_c': 24.1, 'up_c': 23.9,
            'log': ['  0:00  start: duty 40%, window 50 s, '
                    'threshold 14.4 C',
                    '  8:44  bands: flow <= 12.0, no-flow >= 17.6 '
                    '(gap 5.7)'],
            'result': {'flow_rises': [11.6, 12.0, 12.0],
                       'noflow_rises': [17.6, 17.7, 18.1],
                       'flow_max': 12.0, 'noflow_min': 17.6, 'gap': 5.7,
                       'recommend': 14.8},
        }
        self.slots = {
            'booted': 'a',
            'env': {'ffboot_slot': 'a'},
            'slots': {
                'a': {'device': '/dev/mmcblk0p2', 'present': 'yes',
                      'state': 'ok', 'type': 'forgefirm',
                      'version': '20260101000000 (mock)',
                      'kernel': '6.12.0', 'booted': True, 'next': True},
                'b': {'device': '/dev/mmcblk0p3', 'present': 'yes',
                      'state': 'ok', 'type': 'factory', 'version': '2.6.0',
                      'kernel': '4.14', 'booted': False, 'next': False},
            },
            'archives': [{'file': 'factory-rootfs-20260101000000.img.gz',
                          'bytes': 210000000, 'version': '2.6.0',
                          'date': '2026-01-01'}],
            'staged': {'download': {'present': False, 'bytes': 0,
                                    'version': ''},
                       'upload': {'present': False, 'bytes': 0,
                                  'version': ''}},
        }
        self.update = {'running': False, 'kind': '', 'phase': '',
                       'elapsed': 0, 'progress_bytes': -1, 'result': None}
        self.logtext = {lg: '' for lg in ('forgectrl', 'grblhal', 'gfcloud',
                                          'gfhome', 'kernel', 'system')}
        self.logtext['forgectrl'] = (
            'Jan  1 00:00:01 forgectrl: auth: generated a new panel token\n'
            'Jan  1 00:00:01 forgectrl: super: grbl controller started '
            'pid 412\n'
            'Jan  1 00:00:02 forgectrl: cool: engine up, pump on\n')

    # -- helpers
    def _logs_json(self):
        loggers = []
        for lg in ('forgectrl', 'grblhal', 'gfcloud', 'gfhome', 'kernel',
                   'system'):
            d = self.settings.get('log_%s_disk' % lg) or 'info'
            r = self.settings.get('log_%s_remote' % lg) or 'off'
            loggers.append({'name': lg, 'disk': d, 'remote': r,
                            'effective_disk': d, 'effective_remote': r,
                            'bytes': len(self.logtext[lg]),
                            'files': 1 if self.logtext[lg] else 0})
        return {'root': '/data/log/forgefirm',
                'levels': ['off', 'error', 'warning', 'notice', 'info',
                           'debug'],
                'loggers': loggers,
                'syslog_server': self.settings['syslog_server'],
                'syslog_port': self.settings['syslog_port'] or '514',
                'syslog_proto': self.settings['syslog_proto'] or 'udp',
                'effective_syslog_server': self.settings['syslog_server'],
                'effective_known': True, 'pending_reboot': False}

    def _tail_json(self, q):
        name = q.get('name', 'forgectrl')
        text = self.logtext.get(name)
        if text is None:
            return 404, {'error': 'unknown logger'}
        try:
            lines = max(1, int(q.get('lines', '200')))
        except ValueError:
            lines = 200
        try:
            frm = int(q.get('from', '-1'))
        except ValueError:
            frm = -1
        if frm >= 0:
            chunk = text[frm:]
            off = frm
        else:
            parts = text.splitlines(True)[-lines:]
            chunk = ''.join(parts)
            off = len(text) - len(chunk)
        return 200, {'name': name, 'size': len(text), 'offset': off,
                     'text': chunk, 'truncated': False,
                     'exists': bool(text)}

    def _export_targz(self):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as tf:
            for lg, text in self.logtext.items():
                data = text.encode()
                ti = tarfile.TarInfo('forgefirm-logs/%s/%s.log' % (lg, lg))
                ti.size = len(data)
                ti.mtime = int(time.time())
                tf.addfile(ti, io.BytesIO(data))
        return buf.getvalue()

    def _authorized(self, headers, q):
        tok = headers.get('X-ForgeFIRM-Token') or q.get('token')
        return tok == self.token

    def _log(self, line):
        self.logtext['forgectrl'] += 'Jan  1 %s forgectrl: %s\n' % (
            time.strftime('%H:%M:%S'), line)

    # The gate settings as forgectrl's gates.c publishes them (a mirror of
    # its table): legal range, recommended band, off end, and the state of
    # the stored value. The panel renders its warnings from this.
    GATES = (
        ('cool_temp_max', 'coolant_max', 33.0, 5.0, 60.0, 25.0, 38.0, 'high'),
        ('cool_temp_resume', None, 31.0, 5.0, 59.0, 20.0, 36.0, 'none'),
        ('cool_flow_check_s', 'flow', 50.0, 0.0, 300.0, 30.0, 120.0, 'low'),
        ('cool_flow_rise', None, 14.4, 1.0, 40.0, 8.0, 16.0, 'none'),
    )

    def settings_reply(self):
        out = dict(self.settings)
        gates = {}
        for key, gate, default, lo, hi, blo, bhi, off in self.GATES:
            try:
                v = float(self.settings.get(key) or default)
            except ValueError:
                v = default
            if (off == 'low' and v <= lo) or (off == 'high' and v >= hi):
                state = 'off'
            elif v < blo or v > bhi:
                state = 'warn'
            else:
                state = 'ok'
            gates[key] = {'gate': gate, 'def': default, 'lo': lo, 'hi': hi,
                          'band': [blo, bhi], 'off': off, 'value': v,
                          'state': state}
        out['gates'] = gates
        self.status['gates_off'] = [g['gate'] for g in gates.values()
                                    if g['gate'] and g['state'] == 'off']
        return out

    # -- dispatch: returns (status, headers dict, body bytes)
    def handle(self, method, path, q, headers, body):
        with self.lock:
            return self._handle(method, path, q, headers, body)

    def _handle(self, method, path, q, headers, body):
        J = lambda code, obj: (code, {'Content-Type': 'application/json'},
                               json.dumps(obj).encode())
        if method == 'GET':
            if path == '/settings':
                return J(200, self.settings_reply())
            if path == '/status':
                s = dict(self.status)
                s['diag'] = self.diag['running']
                return J(200, s)
            if path == '/mode':
                return J(200, {'mode': self.mode, 'controller': 'running',
                               'pid': 412, 'motion': 'verified'})
            if path == '/cam/status':
                return J(200, {'running': False, 'cam': 'lid', 'clients': 0,
                               'frames': 0, 'fps': 0, 'fps_cap': 0,
                               'encoder': 'vpu', 'buffers': 'cached'})
            if path in ('/cam/snapshot', '/cam/stream') or (
                    path == '/' and q.get('action') in ('snapshot',
                                                        'stream')):
                svg = MOCK_SVG % time.strftime('%H:%M:%S')
                return 200, {'Content-Type': 'image/svg+xml'}, svg.encode()
            if path == '/diag/status':
                d = dict(self.diag)
                if d['running']:
                    d['elapsed_s'] = int(time.time() - self._diag_t0)
                    d['phase'] = 'heating' if d['elapsed_s'] < 20 else \
                        'measuring'
                return J(200, d)
            if path == '/cool/status':
                return J(200, {'state': 'idle', 'pump': True, 'tec': False,
                               'down_c': 22.4, 'up_c': 22.3})
            if path == '/slots':
                return J(200, self.slots)
            if path == '/update/status':
                return J(200, self.update)
            if path == '/logs':
                return J(200, self._logs_json())
            if path == '/logs/tail':
                return J(*self._tail_json(q))
            if path == '/fuse-identity':
                if not self._authorized(headers, q):
                    return J(403, {'error': 'authentication required'})
                return J(200, {'serial': '123456789', 'hostname': 'ABC-123',
                               'password': '0123456789abcdef' * 4})
            return J(404, {'error': 'mock: no such endpoint'})

        if method != 'POST':
            return J(405, {'error': 'method not allowed'})
        if not self._authorized(headers, q):
            return J(403, {'error': 'authentication required'})

        form = dict(q)
        ctype = headers.get('Content-Type', '')
        if body and ctype.startswith('application/x-www-form-urlencoded'):
            form.update(parse_qsl(body.decode('utf-8', 'replace'),
                                  keep_blank_values=True))
        form.pop('token', None)

        if path == '/settings':
            print('mock: POST /settings %s' % json.dumps(form), flush=True)
            for k, v in form.items():
                if k in self.settings and not k.endswith('_set'):
                    self.settings[k] = v
                elif k == 'gf_password':
                    self.settings['gf_password_set'] = bool(v)
            return J(200, self.settings_reply())
        if path == '/mode':
            m = form.get('controller', '')
            if m not in ('grbl', 'gfcloud', 'none'):
                return J(400, {'error': 'controller must be grbl, gfcloud '
                                        'or none'})
            self.mode = m
            self.settings['controller_mode'] = m
            self._log('super: mode -> %s' % m)
            return J(200, {'mode': self.mode, 'controller': 'running',
                           'pid': 413, 'motion': 'unverified'})
        if path in ('/diag/flow-verify', '/diag/flow-calibrate'):
            if self.diag['running']:
                return J(409, {'error': 'a diagnostic is already running'})
            self.diag.update({'running': True, 'tool': path[6:],
                              'phase': 'starting', 'elapsed_s': 0,
                              'log': ['  0:00  start (mock)'],
                              'result': None})
            self._diag_t0 = time.time()
            self._log('diag: %s started' % path[6:])
            return J(200, {'ok': True})
        if path == '/diag/abort':
            self.diag.update({'running': False, 'phase': 'aborted'})
            return J(200, {'ok': True})
        if path == '/boot':
            slot = form.get('slot', '')
            if slot not in ('a', 'b'):
                return J(400, {'error': 'slot must be a or b'})
            for k, s in self.slots['slots'].items():
                s['next'] = (k == slot)
            self.slots['env']['ffboot_slot'] = slot
            return J(200, {'ok': True, 'next': slot})
        if path == '/logs/export':
            data = self._export_targz()
            return 200, {'Content-Type': 'application/gzip',
                         'Content-Disposition':
                         'attachment; filename="forgefirm-logs.tar.gz"'
                         }, data
        if path.startswith('/update/') or path == '/restore/factory':
            self.update.update({'running': False, 'kind': path[1:],
                                'phase': 'done', 'elapsed': 0,
                                'progress_bytes': -1,
                                'result': {'ok': True, 'mock': True}})
            return J(200, {'ok': True, 'mock': True})
        if path in ('/system/reboot', '/controller/stop',
                    '/controller/start', '/cool/state'):
            self._log('%s (mock, no-op)' % path)
            return J(200, {'ok': True})
        return J(404, {'error': 'mock: no such endpoint'})


# ------------------------------------------------------------ server
class BoundedReader:
    """read(n) over a socket file, capped at Content-Length bytes so
    http.client can stream a request body upstream without waiting for
    a socket EOF that keep-alive never delivers."""

    def __init__(self, f, remaining):
        self.f = f
        self.remaining = remaining

    def read(self, n=-1):
        if self.remaining <= 0:
            return b''
        if n is None or n < 0 or n > self.remaining:
            n = self.remaining
        data = self.f.read(n)
        self.remaining -= len(data)
        return data


class Handler(BaseHTTPRequestHandler):
    server_version = 'forgectrl-devserver'
    protocol_version = 'HTTP/1.0'      # one request per connection

    # shared state, set by main()
    cfg = None
    panel = None
    mock = None
    verbose = False
    bundled = False

    # -- entry points
    def do_GET(self):
        self._route()

    def do_POST(self):
        self._route()

    def do_HEAD(self):
        self._route()

    def do_PUT(self):
        self._route()

    def do_DELETE(self):
        self._route()

    def log_message(self, fmt, *args):     # quiet the default access log
        pass

    def _say(self, msg):
        print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)

    # -- routing
    def _route(self):
        u = urlsplit(self.path)
        q = dict(parse_qsl(u.query, keep_blank_values=True))
        try:
            if u.path == '/__dev/watch':
                return self._watch(q)
            if u.path == '/' and 'action' not in q and \
                    self.command in ('GET', 'HEAD'):
                return self._panel()
            if self.command in ('GET', 'HEAD') and not self.bundled and \
                    os.path.splitext(u.path)[1] in CONTENT_TYPES and \
                    self.panel.path(u.path[1:]):
                return self._file(u.path[1:])
            if self.cfg.mock:
                return self._mock(u.path, q)
            return self._proxy(u)
        except ConnectionError:
            pass                       # browser went away; nothing to do

    def _send(self, code, headers, body):
        self.send_response(code)
        for k, v in headers.items():
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, {'Content-Type': 'application/json'},
                   json.dumps(obj).encode())

    def _watch(self, q):
        seen = q.get('v', '')
        v = self.panel.wait_change(seen, WATCH_HOLD_S)
        self._json(200, {'v': v})

    def _token_label(self):
        if self.cfg.mock:
            return self.mock.token, 'mock backend'
        token = self.cfg.token
        label = self.cfg.upstream()[2]
        if not token:
            label += ' (no GF_TOKEN: writes will be refused)'
        return token, label

    def _panel(self):
        token, label = self._token_label()
        try:
            page = self.panel.render(token, label, self.cfg.mock,
                                     self.bundled)
        except (OSError, ValueError) as e:
            self._say('panel: %s' % e)
            return self._send(500, {'Content-Type': 'text/plain'},
                              ('devserver: %s\n' % e).encode())
        if self.verbose:
            self._say('panel: served %d bytes%s'
                      % (len(page), ' (bundled)' if self.bundled else ''))
        self._send(200, {'Content-Type': 'text/html; charset=utf-8'},
                   page.encode('utf-8'))

    def _file(self, name):
        """A file from src/ui/, with the token substituted in text."""
        ext = os.path.splitext(name)[1]
        data = self.panel.read(name)
        if ext in ('.html', '.css', '.js'):
            token = self._token_label()[0]
            data = data.decode('utf-8').replace(TOKEN_MARK, token or '',
                                                1).encode('utf-8')
        if self.verbose:
            self._say('panel: %s (%d B)' % (name, len(data)))
        self._send(200, {'Content-Type': CONTENT_TYPES[ext]}, data)

    def _read_body(self):
        n = int(self.headers.get('Content-Length') or 0)
        return self.rfile.read(n) if n > 0 else b''

    def _mock(self, path, q):
        body = self._read_body() if self.command in ('POST', 'PUT') \
            else b''
        code, hdrs, out = self.mock.handle(self.command, path, q,
                                           self.headers, body)
        if self.verbose or self.command != 'GET' or code >= 400:
            self._say('mock  %s %s -> %d' % (self.command, self.path, code))
        self._send(code, hdrs, out)

    # -- proxy
    def _proxy(self, u):
        ip, port, disp = self.cfg.upstream()
        streaming = u.path in STREAM_PATHS or (
            u.path == '/' and 'action=stream' in u.query)
        timeout = None if streaming else PROXY_TIMEOUT_S

        hdrs = {}
        for k, v in self.headers.items():
            if k.lower() not in DROP_REQ_HEADERS:
                hdrs[k] = v
        hdrs['Host'] = '%s:%d' % (ip, port) if port != 80 else ip
        hdrs['Connection'] = 'close'

        body = None
        n = int(self.headers.get('Content-Length') or 0)
        if n > 0:
            body = BoundedReader(self.rfile, n)
            hdrs['Content-Length'] = str(n)
        elif self.command in ('POST', 'PUT'):
            hdrs['Content-Length'] = '0'

        t0 = time.time()
        conn = http.client.HTTPConnection(ip, port, timeout=timeout)
        try:
            conn.request(self.command, self.path, body=body, headers=hdrs)
            resp = conn.getresponse()
        except (OSError, http.client.HTTPException) as e:
            conn.close()
            self._say('proxy %s %s -> unreachable (%s: %s)'
                      % (self.command, self.path, disp, e))
            return self._json(502, {'error': 'devserver: cannot reach %s '
                                             '(%s)' % (disp, e)})

        self.send_response(resp.status, resp.reason)
        for k, v in resp.getheaders():
            if k.lower() not in DROP_RESP_HEADERS:
                self.send_header(k, v)
        clen = resp.getheader('Content-Length')
        if clen is not None and not resp.chunked:
            self.send_header('Content-Length', clen)
        self.send_header('Connection', 'close')
        self.end_headers()

        sent = 0
        closed = ''
        try:
            if self.command != 'HEAD':
                while True:
                    chunk = resp.read1(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
                    if streaming:
                        self.wfile.flush()
        except ConnectionError:
            closed = ', client closed'    # a stream ended by the browser
        except (OSError, http.client.HTTPException) as e:
            closed = ', upstream error: %s' % e
        finally:
            conn.close()

        if self.verbose or self.command != 'GET' or resp.status >= 400 \
                or streaming:
            self._say('proxy %s %s -> %d %s (%d B, %d ms%s)'
                      % (self.command, self.path, resp.status, resp.reason,
                         sent, int((time.time() - t0) * 1000), closed))


# -------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(
        description='forgectrl panel development server (live reload; '
                    'API proxied to a machine or served by a mock).')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT,
                    help='listen port (default %d)' % DEFAULT_PORT)
    ap.add_argument('--bind', default='127.0.0.1',
                    help='listen address (default 127.0.0.1; use 0.0.0.0 '
                         'to reach it from outside a container)')
    ap.add_argument('--host', help='machine address[:port] (GF_HOST)')
    ap.add_argument('--token', help='panel token (GF_TOKEN)')
    ap.add_argument('--env', help='.env file (default: <repo>/.env)')
    ap.add_argument('--mock', action='store_true',
                    help='answer the API from the built-in mock even if '
                         'GF_HOST is set')
    ap.add_argument('--bundle', action='store_true',
                    help='serve the page bundled (CSS/JS inlined) as the '
                         'daemon does, instead of as three files')
    ap.add_argument('--dump', action='store_true',
                    help='print the bundled page and exit')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='log every request, including the polls')
    args = ap.parse_args()

    panel = Panel(UI_DIR)
    if args.dump:
        try:
            sys.stdout.buffer.write(panel.page(True).encode('utf-8'))
        except (OSError, ValueError) as e:
            sys.exit('devserver: %s' % e)
        return

    cfg = Config(args)
    Handler.cfg = cfg
    Handler.panel = panel
    Handler.mock = Mock(MOCK_TOKEN)
    Handler.verbose = args.verbose
    Handler.bundled = args.bundle

    try:
        size = len(panel.page(True))
        state = '%d bytes bundled, served %s' % (
            size, 'bundled' if args.bundle else 'as files')
    except (OSError, ValueError) as e:
        state = 'ERROR: %s' % e
    print('forgectrl panel dev server')
    print('  panel   %s/ (%s)' % (os.path.relpath(UI_DIR, ROOT)
                                   .replace(os.sep, '/'), state))
    print('  env     %s%s' % (cfg.env_path,
                              '' if os.path.exists(cfg.env_path)
                              else ' (absent)'))
    if cfg.mock:
        print('  backend mock%s' % (' (--mock)' if args.mock else
                                     ' (no GF_HOST configured)'))
    else:
        print('  backend %s%s' % (cfg.upstream()[2],
                                  '' if cfg.token else
                                  '  ** no GF_TOKEN: state-changing calls '
                                  'will be refused **'))
    print('  serve   http://%s:%d/' % (args.bind, args.port), flush=True)

    srv = ThreadingHTTPServer((args.bind, args.port), Handler)
    srv.daemon_threads = True
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Panel development server: live reload for the embedded web UI.

Serves the control panel straight from src/ui.c - the page is
re-extracted from the C string literals on every save, and an injected
watcher reloads the browser tab as soon as the file changes - while
every API request is either

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
                               [--env FILE] [--mock] [-v]
    python3 tools/devserver.py --dump    # extracted HTML to stdout

Then browse http://127.0.0.1:8081 and edit src/ui.c.

Requires only the Python 3 standard library.
"""
import argparse
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
UI_C = os.path.join(ROOT, 'src', 'ui.c')
ENV_FILE = os.path.join(ROOT, '.env')

DEFAULT_PORT = 8081
DEVICE_PORT = 8080
MOCK_TOKEN = '0123456789abcdef0123456789abcdef'
WATCH_POLL_S = 0.25         # ui.c stat interval
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


# ------------------------------------------------------- ui.c panel
class PanelError(Exception):
    pass


ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', 'a': '\a', 'b': '\b',
           'f': '\f', 'v': '\v', '0': '\0', '\\': '\\', '"': '"',
           "'": "'", '?': '?', '/': '/', '\n': ''}


def extract_html(src):
    """Concatenate the C string literals that initialize index_html[],
    honoring the escapes and skipping the comments between pieces - the
    same text the compiler would produce."""
    marker = 'index_html[] ='
    start = src.find(marker)
    if start < 0:
        raise PanelError('no "index_html[] =" in src/ui.c')
    i = start + len(marker)
    n = len(src)
    out = []
    line = src.count('\n', 0, i) + 1

    def where():
        return 'ui.c:%d' % line

    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
        elif c in ' \t\r':
            i += 1
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            if j < 0:
                raise PanelError('%s: unterminated comment' % where())
            line += src.count('\n', i, j)
            i = j + 2
        elif src.startswith('//', i):
            j = src.find('\n', i)
            i = n if j < 0 else j
        elif c == '"':
            i += 1
            while True:
                if i >= n:
                    raise PanelError('%s: unterminated string' % where())
                c = src[i]
                if c == '"':
                    i += 1
                    break
                if c == '\n':
                    raise PanelError('%s: newline inside string literal'
                                     % where())
                if c == '\\':
                    i += 1
                    if i >= n:
                        raise PanelError('%s: dangling backslash'
                                         % where())
                    e = src[i]
                    if e == 'x':
                        j = i + 1
                        while j < n and src[j] in '0123456789abcdefABCDEF':
                            j += 1
                        if j == i + 1:
                            raise PanelError('%s: bad \\x escape'
                                             % where())
                        out.append(chr(int(src[i + 1:j], 16)))
                        i = j
                        continue
                    if e in '01234567':
                        j = i
                        while j < n and j < i + 3 and src[j] in '01234567':
                            j += 1
                        out.append(chr(int(src[i:j], 8)))
                        i = j
                        continue
                    if e not in ESCAPES:
                        raise PanelError('%s: unknown escape \\%s'
                                         % (where(), e))
                    if e == '\n':
                        line += 1
                    out.append(ESCAPES[e])
                    i += 1
                    continue
                out.append(c)
                i += 1
        elif c == ';':
            return ''.join(out)
        else:
            raise PanelError('%s: unexpected %r after string literals'
                             % (where(), c))
    raise PanelError('ui.c: initializer never terminated with ";"')


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
ERROR_HTML = (
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<title>ForgeFIRM - panel extraction error</title></head>"
    "<body style='font:14px/1.5 system-ui,sans-serif;margin:40px;"
    "color:#222'><h2 style='color:#e8262a'>src/ui.c does not extract</h2>"
    "<pre style='background:#f0f1f4;padding:14px;border-radius:6px'>"
    "%(err)s</pre><p style='color:#767a82'>Fix the literal and save; "
    "this page reloads by itself.</p>%(reload)s</body></html>"
)


class Panel:
    """The extracted page, cached by ui.c stamp; a watcher thread bumps
    the version and wakes long-poll waiters when the file changes."""

    def __init__(self, path):
        self.path = path
        self.cond = threading.Condition()
        self.version = self._stamp()
        self._lock = threading.Lock()
        self._cache_key = None
        self._cache = (None, 'not read yet')   # (html, error)
        t = threading.Thread(target=self._watch, daemon=True)
        t.start()

    def _stamp(self):
        try:
            st = os.stat(self.path)
            return '%d-%d' % (st.st_mtime_ns, st.st_size)
        except OSError:
            return 'missing'

    def _watch(self):
        while True:
            time.sleep(WATCH_POLL_S)
            v = self._stamp()
            if v != self.version:
                with self.cond:
                    self.version = v
                    self.cond.notify_all()

    def client_version(self):
        """What the page embeds and polls: the file stamp, marked with
        '!' while the file does not extract - so a fixed literal (or a
        save that completes after a half-written read) reloads the
        error page too."""
        _, err = self.raw()
        return self.version + ('!' if err else '')

    def wait_change(self, seen, timeout):
        """Block until the client version differs from `seen` or the
        timeout passes; returns the current client version. In the
        error state the extraction is retried every second."""
        deadline = time.time() + timeout
        while True:
            cur = self.client_version()
            if cur != seen:
                return cur
            remaining = deadline - time.time()
            if remaining <= 0:
                return cur
            with self.cond:
                self.cond.wait(min(remaining,
                                   1.0 if cur.endswith('!') else remaining))

    def raw(self):
        """(html, error): the extracted page, or the extraction error.
        Cached by file stamp; an error is never cached, so a partial
        read during a save is retried on the next call."""
        with self._lock:
            key = self._stamp()
            if key != self._cache_key or self._cache[1] is not None:
                try:
                    with open(self.path, encoding='utf-8') as f:
                        src = f.read()
                    self._cache = (extract_html(src), None)
                except (OSError, PanelError, ValueError) as e:
                    self._cache = (None, str(e))
                self._cache_key = key
            return self._cache

    def render(self, token, label, mock):
        html, err = self.raw()
        reload_js = RELOAD_JS % {'ver': json.dumps(self.client_version())}
        if err:
            return (ERROR_HTML % {'err': html_escape(err),
                                  'reload': reload_js}), err
        page = html.replace('__FFTOKEN__', token or '')
        badge = BADGE_HTML % {'bg': '#c7760a' if mock else '#3d854d',
                              'label': html_escape(label)}
        inject = badge + reload_js
        k = page.rfind('</body>')
        if k < 0:
            page += inject
        else:
            page = page[:k] + inject + page[k:]
        return page, None


def html_escape(s):
    return (str(s).replace('&', '&amp;').replace('<', '&lt;')
            .replace('>', '&gt;'))


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
            'syslog_server': '', 'syslog_port': '', 'syslog_proto': '',
            'flow_checks_disabled': False,
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

    # -- dispatch: returns (status, headers dict, body bytes)
    def handle(self, method, path, q, headers, body):
        with self.lock:
            return self._handle(method, path, q, headers, body)

    def _handle(self, method, path, q, headers, body):
        J = lambda code, obj: (code, {'Content-Type': 'application/json'},
                               json.dumps(obj).encode())
        if method == 'GET':
            if path == '/settings':
                return J(200, self.settings)
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
            self.settings['flow_checks_disabled'] = \
                self.settings['cool_flow_check_s'] == '0'
            return J(200, self.settings)
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

    def _panel(self):
        if self.cfg.mock:
            token, label = self.mock.token, 'mock backend'
        else:
            token = self.cfg.token
            label = self.cfg.upstream()[2]
            if not token:
                label += ' (no GF_TOKEN: writes will be refused)'
        page, err = self.panel.render(token, label, self.cfg.mock)
        if err:
            self._say('panel: %s' % err)
        elif self.verbose:
            self._say('panel: served %d bytes' % len(page))
        self._send(200 if not err else 500,
                   {'Content-Type': 'text/html; charset=utf-8'},
                   page.encode('utf-8'))

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
    ap.add_argument('--dump', action='store_true',
                    help='print the extracted HTML and exit')
    ap.add_argument('-v', '--verbose', action='store_true',
                    help='log every request, including the polls')
    args = ap.parse_args()

    if args.dump:
        with open(UI_C, encoding='utf-8') as f:
            src = f.read()
        try:
            sys.stdout.write(extract_html(src))
        except PanelError as e:
            sys.exit('devserver: %s' % e)
        return

    cfg = Config(args)
    Handler.cfg = cfg
    Handler.panel = Panel(UI_C)
    Handler.mock = Mock(MOCK_TOKEN)
    Handler.verbose = args.verbose

    html, err = Handler.panel.raw()
    print('forgectrl panel dev server')
    print('  panel   %s (%s)' % (os.path.relpath(UI_C, ROOT),
                                 err or '%d bytes' % len(html)))
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

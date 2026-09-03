#!/usr/bin/env python3
"""Panel development server: live reload for the control panel.

Serves the panel from src/ui/ (index.html, theme.css, help.js, forms.js,
panel.js and the Bootstrap files under vendor/) as plain files - so the
browser's devtools see real file names and line numbers -
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
page the way the daemon does (CSS and JS inlined, one response, before
the daemon's gzip), which is also what --dump prints; the bundling
mirrors src/ui/embed.cmake.

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
# The files index.html links, in load order: the list embed.cmake inlines,
# one marker tag per file.
CSS_FILES = ('vendor/bootstrap.min.css', 'theme.css')
JS_FILES = ('vendor/bootstrap.bundle.min.js', 'help.js', 'forms.js',
            'panel.js')

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


def bundle(html, read):
    """One self-contained page - the same replacement embed.cmake does,
    so `--dump` / `--bundle` show exactly what the daemon serves (before
    its gzip). `read(name)` returns a file under src/ui/ as text."""
    for name in CSS_FILES:
        tag = '<link rel="stylesheet" href="%s" />' % name
        if tag not in html:
            raise ValueError('index.html lacks the marker %s' % tag)
        html = html.replace(tag, '<style>\n' + read(name) + '</style>', 1)
    for name in JS_FILES:
        tag = '<script src="%s"></script>' % name
        if tag not in html:
            raise ValueError('index.html lacks the marker %s' % tag)
        html = html.replace(tag, '<script>\n' + read(name) + '</script>', 1)
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
            p = os.path.join(self.dir, n)
            if os.path.isdir(p):
                try:
                    subs = [n + '/' + m for m in sorted(os.listdir(p))]
                except OSError:
                    continue
            else:
                subs = [n]
            for s in subs:
                try:
                    st = os.stat(os.path.join(self.dir, s))
                except OSError:
                    continue
                parts.append('%s:%d:%d' % (s, st.st_mtime_ns, st.st_size))
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
        """Absolute path of a file in src/ui/ or its vendor/ directory, or
        None if `name` is not a plain file there (no traversal, no
        dotfiles)."""
        parts = name.split('/') if name else []
        if not parts or len(parts) > 2 or (len(parts) == 2 and
                                            parts[0] != 'vendor'):
            return None
        for part in parts:
            if not part or part.startswith('.') or '\\' in part:
                return None
        p = os.path.join(self.dir, *parts)
        return p if os.path.isfile(p) else None

    def read(self, name):
        with open(os.path.join(self.dir, name), 'rb') as f:
            return f.read()

    def text(self, name):
        return self.read(name).decode('utf-8')

    def page(self, bundled):
        """index.html as text: the files linked (dev default, real file
        names and line numbers in the browser) or inlined."""
        html = self.text('index.html')
        if bundled:
            html = bundle(html, self.text)
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

# The settings keys forgectrl serves, in the order of its setting_defs
# table (src/main.c). GET /settings reports a secret key as "<key>_set"
# (a boolean) and never its value; POST accepts it under its own name.
# tests/test_devserver_mock.py holds both tuples to the C table.
SETTINGS_KEYS = (
    'controller_mode', 'homing_mode',
    'gfcloud_home_x', 'gfcloud_home_y', 'gfcloud_home_z',
    'gfcloud_home_timeout_s', 'gf_serial', 'gf_password', 'ui_units',
    'wifi_country',
    'cool_flow_rise', 'cool_flow_heater_pct', 'cool_flow_check_s',
    'cool_recheck_s', 'cool_confirm_max_s', 'cool_laser_heat_cw',
    'cool_laser_heat_density', 'cool_aa_offset_counts',
    'cool_temp_max', 'cool_temp_resume', 'cool_temp_critical_c',
    'cool_temp_min', 'cool_temp_start', 'cool_tec_present',
    'cool_tec_on_c', 'cool_tec_off_c',
    'cool_fire_q1_alert', 'cool_fire_q1_critical',
    'cool_fire_q2_alert', 'cool_fire_q2_critical',
    'cool_accel_x_alert', 'cool_accel_y_alert', 'cool_accel_abort',
    'cool_cooldown_s', 'cool_cooldown_max_s',
    'cool_tach_exhaust_min_rpm', 'cool_tach_intake_min_rpm',
    'cool_tach_air_assist_min_rpm', 'cool_purge_min_current',
    'cool_fan_grace_s',
    'laser_button_timeout_s', 'laser_disarm_s', 'laser_floor_density',
    'laser_dose_curve', 'laser_corner_gamma', 'laser_pulse_ticks',
    'laser_pulse_min_ticks', 'rail_settle_s', 'lid_lamp_idle',
    'cloud_pause_backtrack_ticks', 'cloud_resume_lead_ticks',
    'cloud_hold_max_s', 'pulse_warn_threshold_bytes',
    'pulse_reject_threshold_bytes', 'lid_policy',
    'log_forgectrl_disk', 'log_forgectrl_remote',
    'log_grblhal_disk', 'log_grblhal_remote',
    'log_gfcloud_disk', 'log_gfcloud_remote',
    'log_gfhome_disk', 'log_gfhome_remote',
    'log_kernel_disk', 'log_kernel_remote',
    'log_system_disk', 'log_system_remote',
    'syslog_server', 'syslog_port', 'syslog_proto',
)
SECRET_KEYS = ('gf_password',)
# The enumerated keys the daemon's validators accept (the numeric keys
# outside the gate table are not range-checked here).
SETTING_CHOICES = {
    'controller_mode': ('grbl', 'cloud'),
    'homing_mode': ('none', 'gfcloud', 'switches'),
    'ui_units': ('metric', 'imperial'),
    'cool_tec_present': ('0', '1'),
    'lid_policy': ('cancel', 'hold'),
    'syslog_proto': ('udp', 'tcp'),
}
LOGGERS = ('forgectrl', 'grblhal', 'gfcloud', 'gfhome', 'kernel', 'system')
LOG_LEVELS = ('off', 'error', 'warning', 'notice', 'info', 'debug')
LOGS_ROOT = '/data/log/forgefirm'
FAN_NAMES = ('exhaust', 'intake_1', 'intake_2', 'air_assist', 'purge')
FIRE_GATES = ('cool_fire_q1_alert', 'cool_fire_q1_critical',
              'cool_fire_q2_alert', 'cool_fire_q2_critical')
DIAG_TOOLS = ('flow-verify', 'flow-calibrate', 'aa-offset-calibrate')
SLOT_TARGETS = {'sd': '/dev/mmcblk1p1', 'a': '/dev/mmcblk2p1',
                'b': '/dev/mmcblk2p2', 'legacy': '/dev/mmcblk2p4'}
MOCK_VERSION = '20260101000000 (mock)'
MOCK_RELEASE = '0.0.2'          # what /update/check offers
JOB_S = 8                       # seconds a mock update job runs
CURVE_WAIT_S, CURVE_RECORD_S = 3, 12
# A mock update job walks its kind's phases (the daemon's own strings)
# evenly over JOB_S seconds.
JOB_PHASES = {
    'download': ('downloading', 'verifying signature'),
    'apply': ('taking update lock', 'verifying archive',
              'unmounting target', 'writing slot', 'verifying written slot'),
    'restore': ('taking update lock', 'verifying archive checksum',
                'unmounting target', 'writing factory image',
                'verifying written slot'),
}

# A mock diagnostic walks these phases (the daemon's own phase strings)
# by elapsed second, then ends with the canned result for its tool.
DIAG_PLANS = {
    'flow-verify': (
        (3, 'stopping the motion controller'),
        (10, 'settling before trial 1/1 (pump on)'),
        (20, 'trial 1/1: heater 40% for 50 s (pump on)'),
        (27, 'settling before trial 1/1 (pump off)'),
        (37, 'trial 1/1: heater 40% for 50 s (pump off)'),
        (40, 'standing down'),
    ),
    'flow-calibrate': (
        (3, 'stopping the motion controller'),
        (8, 'settling before trial 1/3 (pump on)'),
        (14, 'trial 1/3: heater 40% for 50 s (pump on)'),
        (20, 'trial 1/3: heater 40% for 50 s (pump off)'),
        (26, 'trial 2/3: heater 40% for 50 s (pump on)'),
        (32, 'trial 2/3: heater 40% for 50 s (pump off)'),
        (38, 'trial 3/3: heater 40% for 50 s (pump on)'),
        (44, 'trial 3/3: heater 40% for 50 s (pump off)'),
        (47, 'standing down'),
    ),
    'aa-offset-calibrate': (
        (3, 'stopping the motion controller'),
        (10, 'settling with the fans idle'),
        (16, 'cycle 1/3: air assist to run'),
        (22, 'cycle 1/3: air assist to idle'),
        (28, 'cycle 2/3: air assist to run'),
        (34, 'cycle 2/3: air assist to idle'),
        (40, 'cycle 3/3: air assist to run'),
        (46, 'cycle 3/3: air assist to idle'),
        (49, 'standing down'),
    ),
}
DIAG_RESULTS = {
    'flow-verify': {'pass': True, 'threshold': 14.4, 'flow_rise': 11.8,
                    'flow_dt': 2.1, 'noflow_rise': 17.6, 'noflow_dt': 7.9,
                    'margin_flow': 2.6, 'margin_noflow': 3.2,
                    'thin_margin': False},
    'flow-calibrate': {'flow_rises': [11.6, 12.0, 12.0],
                       'noflow_rises': [17.6, 17.7, 18.1],
                       'flow_max': 12.0, 'noflow_min': 17.6, 'gap': 5.6,
                       'recommend': 14.8},
    'aa-offset-calibrate': {'steps': [[-19.5, -20.1], [20.3, 19.8],
                                      [-19.9, -20.4], [20.0, 19.6],
                                      [-20.2, -19.7], [19.8, 20.3]],
                            'offset_counts': 19.9, 'spread_counts': 0.8,
                            'recommend': 19.9},
}


def num(x):
    """A float as the daemon's %g prints it: 33 stays 33, 14.4 stays
    14.4 (JSON reads both as numbers; the text just matches)."""
    return int(x) if float(x) == int(x) else x


class Mock:
    """In-memory stand-in for the daemon: every endpoint it registers,
    in the JSON shape it serves, so the JavaScript runs end to end.
    State-changing calls are token-checked like the real thing, so a
    missing fx() shows up as the same 403 it would on the machine, and
    refusals come back the way the daemon sends them: text/plain from
    the handlers in main.c, {"error":...} from the update module and
    the token check.

    GF_MOCK_BUTTON=1 in the environment holds the machine button down
    (the fuse-identity viewer needs an operator at the machine)."""

    # The gate settings, one row per line of the table in src/gates.c:
    # key, the gate name /status reports (None for a key that tunes a
    # gate without being one), default, legal range, recommended band,
    # and which end of the range turns the gate off.
    # tests/test_devserver_mock.py holds this to the C table.
    GATES = (
        ('cool_temp_max', 'coolant_max', 33.0, 5.0, 60.0, 25.0, 38.0, 'high'),
        ('cool_temp_resume', None, 31.0, 5.0, 59.0, 20.0, 36.0, 'none'),
        ('cool_temp_critical_c', 'coolant_critical', 38.0, 6.0, 70.0, 36.0, 45.0, 'high'),
        ('cool_temp_min', 'coolant_min', 5.0, 0.0, 40.0, 3.0, 8.0, 'low'),
        ('cool_temp_start', 'warm_up', 16.0, 0.0, 40.0, 12.0, 20.0, 'low'),
        ('cool_tec_on_c', None, 20.0, 6.0, 32.0, 18.0, 24.0, 'none'),
        ('cool_tec_off_c', None, 18.0, 5.0, 31.0, 16.0, 22.0, 'none'),
        ('cool_fire_q1_alert', 'flame_q1_alert', 275.0, 0.0, 1023.0, 250.0, 450.0, 'low'),
        ('cool_fire_q1_critical', 'flame_q1_critical', 688.0, 0.0, 1023.0, 500.0, 1023.0, 'low'),
        ('cool_fire_q2_alert', 'flame_q2_alert', 374.0, 0.0, 1023.0, 300.0, 500.0, 'low'),
        ('cool_fire_q2_critical', 'flame_q2_critical', 1022.0, 0.0, 1023.0, 500.0, 1023.0, 'low'),
        ('cool_accel_x_alert', 'crash_x_alert', 132.0, 0.0, 255.0, 100.0, 170.0, 'low'),
        ('cool_accel_y_alert', 'crash_y_alert', 112.0, 0.0, 255.0, 85.0, 145.0, 'low'),
        ('cool_accel_abort', 'crash_abort', 133.0, 0.0, 255.0, 100.0, 170.0, 'low'),
        ('cool_flow_check_s', 'flow', 50.0, 0.0, 300.0, 30.0, 120.0, 'low'),
        ('cool_recheck_s', 'recheck', 150.0, 0.0, 3600.0, 60.0, 600.0, 'low'),
        ('cool_flow_rise', None, 14.4, 1.0, 40.0, 8.0, 16.0, 'none'),
        ('cool_tach_exhaust_min_rpm', 'exhaust', 6400.0, 0.0, 20000.0, 5800.0, 7000.0, 'low'),
        ('cool_tach_intake_min_rpm', 'intake', 2290.0, 0.0, 20000.0, 2100.0, 2500.0, 'low'),
        ('cool_tach_air_assist_min_rpm', 'air_assist', 6000.0, 0.0, 30000.0, 5500.0, 6600.0, 'low'),
        ('cool_purge_min_current', 'purge', 300.0, 0.0, 1023.0, 150.0, 500.0, 'low'),
        ('cool_fan_grace_s', None, 15.0, 0.0, 120.0, 5.0, 30.0, 'none'),
    )

    def __init__(self, token):
        self.token = token
        self.lock = threading.Lock()
        self.t0 = time.time()
        self.version = MOCK_VERSION
        self.machine_id = 'ABC-123'
        self.settings = dict.fromkeys(SETTINGS_KEYS, '')
        self.settings.update({'controller_mode': 'grbl',
                              'homing_mode': 'gfcloud'})
        # The supervisor (GET /mode): the selected mode, the controller
        # process state, its pid and the motion-liveness probe.
        self.mode = 'grbl'
        self.controller = 'running'
        self.pid = 412
        self.motion = 'verified'
        self.report_at = self.t0
        self.rep_armed = False
        self.button = os.environ.get('GF_MOCK_BUTTON') == '1'
        # GET /status, in the daemon's key order; diag, grbl and
        # gates_off are filled in per request.
        self.status = {
            'state': 'idle', 'homed': True, 'diag': False,
            'pos': {'x': 12.34, 'y': -5.6, 'z': 0.0},
            'laser_locked': True,
            'laser': {'emission_samples': 0, 'pgood_samples': 255},
            'faults': 0, 'hv_current_raw': 0, 'lid_ir': [2, 2, 3, 2],
            'fans': {'air_assist': 1990, 'exhaust': 0, 'intake_1': 720,
                     'intake_2': 730},
            'coolant': {'down_c': 22.4, 'up_c': 22.3, 'pump': True,
                        'tec': False},
            'temps': {'chassis_c': 29.0, 'supply_raw': 589, 'soc_c': 42.8,
                      'soc_throttle': 0},
            'sys': {'cpu_pct': 7.4, 'mem_pct': 38.2},
            'gfsvc': {'latest': '2.6.0', 'tested': '2.6.0'},
            'switches': {'lid': True, 'button': False, 'interlock_ok': True,
                         'head': True, 'hv_enable': False},
        }
        # The controller's grbl.state report, embedded verbatim by the
        # daemon while the GRBL controller runs (ts_mono is stamped per
        # request).
        self.grbl_report = {
            'state': 'Idle', 'alarm': 0,
            'sender': {'connected': False, 'generation': 3, 'for_s': 0,
                       'peer': ''},
            'laser': {'armed': False, 'arming': False, 'model': 'density',
                      'floor_pct': 10, 'curve': 'off'},
            'modals': '[GC:G0 G54 G17 G21 G91 G94 M5 M9 T0 F600 S500.]',
            'overrides': {'feed': 100, 'rapid': 100},
            'driver': '260809',
        }
        self.diag = {
            'running': False, 'tool': 'flow-calibrate', 'phase': 'done',
            'elapsed_s': 0, 'down_c': 24.1, 'up_c': 23.9,
            'log': ['  0:00  start: duty 40%, window 50 s, '
                    'threshold 14.4 C',
                    '  8:44  bands: flow <= 12.0, no-flow >= 17.6 '
                    '(gap 5.6)',
                    '  8:47  done'],
            'result': DIAG_RESULTS['flow-calibrate'],
        }
        self.diag_t0 = 0.0
        self.slots = {
            'booted': SLOT_TARGETS['a'],
            'env': {'mmcdev': '2', 'mmchwpart': '0', 'mmcpart': '1',
                    'mmcroot': SLOT_TARGETS['a']},
            'slots': {
                'a': {'device': SLOT_TARGETS['a'], 'present': 'yes',
                      'state': 'ok', 'type': 'forgefirm',
                      'version': MOCK_VERSION, 'kernel': 'yes',
                      'booted': True, 'next': True},
                'b': {'device': SLOT_TARGETS['b'], 'present': 'yes',
                      'state': 'ok', 'type': 'factory', 'version': 'v2.6.0',
                      'kernel': 'yes', 'booted': False, 'next': False},
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
        self.job_t0 = 0.0
        self.job_args = {}
        self.curve = {'state': 'idle', 'reason': '', 'elapsed_s': 0,
                      'samples': 0, 'curve': '', 'points': []}
        self.curve_t0 = 0.0
        self.logtext = {lg: '' for lg in LOGGERS}
        self.logtext['forgectrl'] = (
            'Jan  1 00:00:01 forgectrl: auth: generated a new panel token\n'
            'Jan  1 00:00:01 forgectrl: super: grbl controller started '
            'pid 412\n'
            'Jan  1 00:00:02 forgectrl: cool: engine up, pump on\n')

    # -- gates (the table above, read the way gates.c reads it)
    def gate_row(self, key):
        for row in self.GATES:
            if row[0] == key:
                return row
        return None

    def gate_value(self, row):
        """The stored value inside its legal range, else the default
        (gate_parse: an empty or out-of-range value is no value)."""
        try:
            v = float(self.settings.get(row[0]) or '')
        except ValueError:
            return row[2]
        return v if row[3] <= v <= row[4] else row[2]

    def gate(self, key):
        return self.gate_value(self.gate_row(key))

    @staticmethod
    def gate_state(row, v):
        """gate_state in gates.c: off wins over warn."""
        lo, hi, blo, bhi, off = row[3:]
        if (off == 'low' and v <= lo) or (off == 'high' and v >= hi):
            return 'off'
        if v < blo or v > bhi:
            return 'warn'
        return 'ok'

    def gates_json(self):
        out = {}
        for row in self.GATES:
            key, gate, default, lo, hi, blo, bhi, off = row
            v = self.gate_value(row)
            out[key] = {'gate': gate, 'def': num(default), 'lo': num(lo),
                        'hi': num(hi), 'band': [num(blo), num(bhi)],
                        'off': off, 'value': num(v),
                        'state': self.gate_state(row, v)}
        return out

    def gates_off(self):
        return [row[1] for row in self.GATES
                if row[1] and self.gate_state(row, self.gate_value(row))
                == 'off']

    # -- documents
    def settings_reply(self):
        out = {}
        for k in SETTINGS_KEYS:
            if k in SECRET_KEYS:
                out[k + '_set'] = bool(self.settings[k])
            else:
                out[k] = self.settings[k]
        out['gates'] = self.gates_json()
        out['version'] = self.version
        out['machine_id'] = self.machine_id
        return out

    def setting_valid(self, key, v):
        """The daemon's validators, as far as the mock mirrors them: the
        gate table's legal ranges, the enumerated keys and the log
        levels. The other numeric keys pass unchecked."""
        row = self.gate_row(key)
        if row:
            try:
                f = float(v)
            except ValueError:
                return False
            return row[3] <= f <= row[4]
        if key in SETTING_CHOICES:
            return v in SETTING_CHOICES[key]
        if key.startswith('log_'):
            return v in LOG_LEVELS
        return True

    def idle(self):
        return self.status['state'] == 'idle'

    def mode_reply(self):
        return {'mode': self.mode, 'controller': self.controller,
                'pid': self.pid, 'motion': self.motion}

    def status_reply(self):
        self.status['diag'] = self.diag['running']
        self.status['switches']['button'] = self.button
        grbl = None
        if self.mode == 'grbl' and self.controller == 'running':
            report = {'ts_mono': round(time.monotonic() - 1.2, 3)}
            report.update(self.grbl_report)
            grbl = {'age_s': 1.2, 'report': report}
        out = {}
        for k, v in self.status.items():
            if k == 'temps' and grbl:
                out['grbl'] = grbl
            if k == 'switches':
                out['gates_off'] = self.gates_off()
            out[k] = v
        return out

    def cool_reply(self):
        fans = self.status['fans']
        readings = dict(fans, purge=627)
        floors = {'exhaust': self.gate('cool_tach_exhaust_min_rpm'),
                  'intake_1': self.gate('cool_tach_intake_min_rpm'),
                  'intake_2': self.gate('cool_tach_intake_min_rpm'),
                  'air_assist': self.gate('cool_tach_air_assist_min_rpm'),
                  'purge': self.gate('cool_purge_min_current')}
        fan_gates = {}
        for n in FAN_NAMES:
            fan_gates[n] = {'reading': readings[n], 'floor': num(floors[n]),
                            'state': 'off' if floors[n] <= 0 else 'idle'}
        age = -1 if self.report_at is None else \
            round(time.time() - self.report_at, 1)
        watching = any(self.gate(k) > 0 for k in FIRE_GATES)
        c = self.status['coolant']
        return {
            'phase': 'idle', 'verdict': 'OK', 'fire_ok': True,
            'hold': False, 'resume_ok': True, 'reason': '',
            'down_c': c['down_c'], 'up_c': c['up_c'],
            'report_age_s': age, 'armed': self.rep_armed,
            'fire_watch': 'armed' if watching else 'watch',
            'accel_watch': 'watch',
            'gates_off': self.gates_off(),
            'limits': {
                'coolant_max_c': self.gate('cool_temp_max'),
                'coolant_resume_c': self.gate('cool_temp_resume'),
                'coolant_critical_c': self.gate('cool_temp_critical_c'),
                'coolant_source': 'local',
                'coolant_min_c': self.gate('cool_temp_min'),
                'exhaust_min_rpm': num(floors['exhaust']),
                'intake_min_rpm': num(floors['intake_1']),
                'air_assist_min_rpm': num(floors['air_assist'])},
            'fan_gates': fan_gates}

    def cam_reply(self):
        return {'running': False, 'cam': 'lid', 'clients': 0, 'frames': 0,
                'fps': 0.0, 'fps_cap': 15.0, 'hw_fps_skip': False,
                'encoder': 'vpu', 'convert': 'gpu', 'buffers': 'cached',
                'sensor': 'OV5648',
                'stream': {'width': 1296, 'height': 972},
                'snapshot': {'width': 2592, 'height': 1944},
                'h264': {'active': False, 'clients': 0},
                'health': {'captured': 0, 'corrupt': 0, 'restarts': 0},
                'capture_allowed': self.status['switches']['lid'],
                'stopped_by_lid': False}

    def _logs_json(self):
        loggers = []
        for lg in LOGGERS:
            d = self.settings.get('log_%s_disk' % lg) or 'info'
            r = self.settings.get('log_%s_remote' % lg) or 'off'
            loggers.append({'name': lg, 'disk': d, 'remote': r,
                            'effective_disk': d, 'effective_remote': r,
                            'bytes': len(self.logtext[lg]),
                            'files': 1 if self.logtext[lg] else 0})
        return {'root': LOGS_ROOT, 'levels': list(LOG_LEVELS),
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
            return None
        try:
            lines = max(1, int(q.get('lines', '200')))
        except ValueError:
            lines = 200
        try:
            frm = int(q.get('from', '-1'))
        except ValueError:
            frm = -1
        if not text:
            return {'name': name, 'size': 0, 'offset': 0, 'text': '',
                    'truncated': False, 'exists': False}
        if frm >= 0:
            chunk = text[frm:]
            off = frm
        else:
            parts = text.splitlines(True)[-lines:]
            chunk = ''.join(parts)
            off = len(text) - len(chunk)
        return {'name': name, 'size': len(text), 'offset': off,
                'text': chunk, 'truncated': False, 'exists': True}

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

    # -- the clock: mock jobs advance by wall time on every request
    def _diag_log(self, el, line):
        self.diag['log'].append('%3d:%02d  %s' % (el // 60, el % 60, line))

    def _diag_end(self, el, result):
        self._diag_log(el, 'done')
        self.diag.update({'running': False, 'phase': 'done',
                          'elapsed_s': 0, 'result': result})
        self.controller = 'running'
        self.pid += 1
        self._log('diag: %s done' % self.diag['tool'])

    def _tick(self):
        now = time.time()
        d = self.diag
        if d['running']:
            el = int(now - self.diag_t0)
            d['elapsed_s'] = el
            phase = None
            for t_end, name in DIAG_PLANS[d['tool']]:
                if el < t_end:
                    phase = name
                    break
            if phase is None:
                self._diag_end(el, DIAG_RESULTS[d['tool']])
            elif phase != d['phase']:
                d['phase'] = phase
                self._diag_log(el, phase)
        u = self.update
        if u['running']:
            el = int(now - self.job_t0)
            u['elapsed'] = el
            phases = JOB_PHASES[u['kind']]
            u['phase'] = phases[min(el * len(phases) // JOB_S,
                                    len(phases) - 1)]
            if u['kind'] != 'download':
                u['progress_bytes'] = min(el, JOB_S) * 25000000
            if el >= JOB_S:
                self._job_end()
        c = self.curve
        if c['state'] in ('waiting', 'recording'):
            el = int(now - self.curve_t0)
            c['elapsed_s'] = el
            if c['state'] == 'waiting' and el >= CURVE_WAIT_S:
                c['state'] = 'recording'
            if c['state'] == 'recording':
                c['samples'] = (el - CURVE_WAIT_S) * 8
                if el >= CURVE_WAIT_S + CURVE_RECORD_S:
                    c.update({'state': 'done', 'elapsed_s': 0,
                              'curve': '10:0.5,20:3,40:18,60:44,80:72,100:100',
                              'points': [{'density': 10, 'light': 0.5},
                                         {'density': 20, 'light': 3.0},
                                         {'density': 40, 'light': 18.0},
                                         {'density': 60, 'light': 44.0},
                                         {'density': 80, 'light': 72.0},
                                         {'density': 100, 'light': 100.0}]})

    def _job_end(self):
        u, a = self.update, self.job_args
        slots = self.slots['slots']
        if u['kind'] == 'download':
            self.slots['staged']['download'] = {
                'present': True, 'bytes': 87654321, 'version': MOCK_RELEASE}
            result = {'ok': True, 'file': 'download',
                      'version': MOCK_RELEASE, 'bytes': 87654321}
        elif u['kind'] == 'apply':
            ver = self.slots['staged'][a['file']]['version']
            slots[a['slot']].update({'type': 'forgefirm', 'version': ver,
                                     'state': 'ok', 'kernel': 'yes'})
            result = {'ok': True, 'slot': a['slot'], 'version': ver,
                      'signed': True}
        else:
            ver = a['archive']['version']
            slots[a['slot']].update({'type': 'factory', 'version': 'v' + ver,
                                     'state': 'ok', 'kernel': 'yes'})
            result = {'ok': True, 'slot': a['slot'], 'factory_version': ver}
        u.update({'running': False, 'phase': '', 'elapsed': 0,
                  'progress_bytes': -1, 'result': result})
        self._log('update: %s done' % u['kind'])

    def _job_start(self, kind, args, J):
        if self.update['running']:
            return J(409, {'error': 'an update job is already running'})
        if not self.idle():
            return J(409, {'error': 'machine is not idle'})
        if self.diag['running']:
            return J(409, {'error': 'a diagnostic is running'})
        self.update.update({'running': True, 'kind': kind,
                            'phase': JOB_PHASES[kind][0], 'elapsed': 0,
                            'progress_bytes': -1, 'result': None})
        self.job_t0 = time.time()
        self.job_args = args
        self._log('update: %s started' % kind)
        return J(202, {'started': True})

    def _slot_target(self, form, J):
        """The a/b slot an apply or restore writes, or the daemon's
        refusal (the booted slot and the next-boot slot are never
        written)."""
        slot = form.get('slot', '')
        s = self.slots['slots'].get(slot)
        if slot not in ('a', 'b') or not s:
            return None, J(400, {'error': 'slot must be a or b'})
        if s['booted']:
            return None, J(409, {'error':
                                 'refusing to write the booted root slot'})
        if s['next']:
            return None, J(409, {'error':
                                 'refusing to write the slot selected for '
                                 'the next boot - point the next boot back '
                                 'at the running slot first'})
        return slot, None

    # -- dispatch: returns (status, headers dict, body bytes)
    def handle(self, method, path, q, headers, body):
        with self.lock:
            self._tick()
            return self._handle(method, path, q, headers, body)

    def _handle(self, method, path, q, headers, body):
        J = lambda code, obj: (code, {'Content-Type': 'application/json'},
                               json.dumps(obj).encode())
        T = lambda code, msg: (code, {'Content-Type': 'text/plain'},
                               msg.encode())
        if method == 'GET':
            if path == '/settings':
                return J(200, self.settings_reply())
            if path == '/status':
                return J(200, self.status_reply())
            if path == '/mode':
                return J(200, self.mode_reply())
            if path == '/cam/status':
                return J(200, self.cam_reply())
            if path in ('/cam/snapshot', '/cam/stream') or (
                    path == '/' and q.get('action') in ('snapshot',
                                                        'stream')):
                svg = MOCK_SVG % time.strftime('%H:%M:%S')
                return 200, {'Content-Type': 'image/svg+xml'}, svg.encode()
            if path == '/cam/h264':
                return T(503, 'H.264 stream unavailable (the MJPEG stream '
                              'still works)')
            if path == '/grbl/settings':
                if not (self.mode == 'grbl' and self.controller == 'running'):
                    return T(404, 'no grbl controller')
                return T(200, '$0=10\n$1=255\n$32=1\n$35=10\n')
            if path == '/curve/status':
                return J(200, self.curve)
            if path == '/curve/ladder.gcode':
                return 200, {'Content-Type': 'text/plain',
                             'Content-Disposition':
                             'attachment; filename="dose-ladder.gcode"'
                             }, b'; mock ladder\nG21\nM3\nS1000\nM5\n'
            if path == '/diag/status':
                return J(200, self.diag)
            if path == '/cool/status':
                return J(200, self.cool_reply())
            if path == '/slots':
                return J(200, self.slots)
            if path == '/update/status':
                return J(200, self.update)
            # Token-gated reads: log content and the fuse identity.
            if path in ('/logs', '/logs/tail', '/fuse-identity'):
                if not self._authorized(headers, q):
                    return J(403, {'error': 'authentication required'})
            if path == '/logs':
                return J(200, self._logs_json())
            if path == '/logs/tail':
                if 'name' not in q:
                    return T(400, 'name required')
                tail = self._tail_json(q)
                if tail is None:
                    return T(404, 'unknown logger')
                return J(200, tail)
            if path == '/fuse-identity':
                if not self.button:
                    return T(403, 'hold the machine button to reveal the '
                                  'fuse identity')
                return J(200, {'serial': '123456789', 'hostname': 'ABC-123',
                               'password': '0123456789abcdef' * 4})
            return J(404, {'error': 'mock: no such endpoint'})

        if method != 'POST':
            return J(405, {'error': 'method not allowed'})

        form = dict(q)
        ctype = headers.get('Content-Type', '')
        if body and ctype.startswith('application/x-www-form-urlencoded'):
            form.update(parse_qsl(body.decode('utf-8', 'replace'),
                                  keep_blank_values=True))
        form.pop('token', None)

        # The controller's job-state report: loopback-only on the
        # machine, no token.
        if path == '/cool/state':
            mode = form.get('mode')
            if mode is None:
                return T(400, 'mode is required')
            if mode not in ('idle', 'run', 'cooldown'):
                return T(400, 'mode must be idle, run or cooldown')
            self.report_at = time.time()
            self.rep_armed = form.get('armed', '0') not in ('', '0')
            return J(200, {'ok': True})

        if not self._authorized(headers, q):
            return J(403, {'error': 'authentication required'})

        if path == '/settings':
            print('mock: POST /settings %s' % json.dumps(form), flush=True)
            if self.diag['running']:
                return T(409, 'a diagnostic is running - settings are '
                              'locked')
            if not self.idle():
                return T(409, 'machine is not idle - settings are locked')
            known = [k for k in SETTINGS_KEYS if k in form]
            if not known:
                return T(400, 'no known setting in request')
            for k in known:
                if form[k] and not self.setting_valid(k, form[k]):
                    return T(400, 'invalid value for %s' % k)
            for k in known:
                self.settings[k] = form[k]
                self._log('%s %s' % (k, 'cleared' if not form[k] else
                                     'set' if k in SECRET_KEYS else form[k]))
            return J(200, self.settings_reply())
        if path == '/mode':
            m = form.get('controller')
            if m is None:
                return T(400, 'controller is required')
            if m not in ('grbl', 'cloud'):
                return T(400, 'mode must be grbl or cloud')
            if self.diag['running']:
                return T(409, 'a diagnostic is running')
            if self.update['running']:
                return T(409, 'an update job is running')
            if not self.idle():
                return T(409, 'machine is not idle')
            self.mode = m
            self.settings['controller_mode'] = m
            self.controller = 'running'
            self.pid += 1
            self.motion = 'unverified'
            self._log('super: mode -> %s' % m)
            return J(200, self.mode_reply())
        if path == '/controller/stop':
            self.controller = 'standby'
            self.pid = 0
            self.report_at = None
            self._log('super: controller stopped')
            return J(200, {'stopped': True})
        if path == '/controller/start':
            if self.diag['running']:
                return T(409, 'a diagnostic owns the hardware')
            self.controller = 'running'
            self.pid += 1
            self.report_at = time.time()
            self._log('super: controller started pid %d' % self.pid)
            return J(200, {'started': True})
        if path.startswith('/diag/') and path != '/diag/abort':
            tool = path[6:]
            if self.update['running']:
                return T(409, 'an update job is running')
            if tool not in DIAG_TOOLS:
                return T(400, 'unknown diagnostic')
            if self.diag['running']:
                return T(409, 'a diagnostic is already running')
            if not self.idle():
                return T(409, 'machine is not idle')
            self.diag.update({'running': True, 'tool': tool, 'phase': '',
                              'elapsed_s': 0, 'log': [], 'result': None})
            self.diag_t0 = time.time()
            self.controller = 'standby'
            self.pid = 0
            self._log('diag: %s started' % tool)
            return J(202, {'started': True})
        if path == '/diag/abort':
            if self.diag['running']:
                self._diag_end(self.diag['elapsed_s'],
                               {'error': 'aborted by operator'})
            return J(200, {'aborting': True})
        if path == '/curve/record':
            if self.curve['state'] in ('waiting', 'recording'):
                return T(409, 'a recording is already running')
            if self.grbl_report['sender']['connected']:
                return T(409, 'a sender is connected to the machine - close '
                              'it before recording')
            self.curve.update({'state': 'waiting', 'reason': '',
                               'elapsed_s': 0, 'samples': 0, 'curve': '',
                               'points': []})
            self.curve_t0 = time.time()
            return J(200, self.curve)
        if path == '/curve/stop':
            if self.curve['state'] == 'waiting':
                self.curve.update({'state': 'failed', 'elapsed_s': 0,
                                   'reason': 'stopped before the ladder '
                                             'fired'})
            elif self.curve['state'] == 'recording':
                self.curve_t0 -= CURVE_RECORD_S
                self._tick()
            return J(200, self.curve)
        if path == '/boot':
            if self.diag['running']:
                return J(409, {'error': 'a diagnostic is running'})
            if not self.idle():
                return J(409, {'error': 'machine is not idle'})
            if self.update['running']:
                return J(409, {'error': 'an update job is running'})
            t = form.get('target', '')
            if t not in SLOT_TARGETS:
                return J(400, {'error': 'target must be sd, a, b, or legacy'})
            if t not in self.slots['slots']:
                return J(409, {'error': 'boot selection failed',
                               'detail': '%s is not present' % t})
            for k, s in self.slots['slots'].items():
                s['next'] = (k == t)
            self.slots['env']['mmcroot'] = SLOT_TARGETS[t]
            return J(200, {'ok': True, 'target': t,
                           'detail': 'next boot: %s' % SLOT_TARGETS[t]})
        if path == '/system/reboot':
            if self.diag['running']:
                return J(409, {'error': 'a diagnostic is running'})
            if not self.idle():
                return J(409, {'error': 'machine is not idle'})
            if self.update['running']:
                return J(409, {'error': 'an update job is running'})
            if form.get('confirm') != '1':
                return J(400, {'error': 'confirm=1 required'})
            self._log('system: reboot requested (mock, no-op)')
            return J(200, {'rebooting': True})
        if path == '/update/check':
            return J(200, {'available': True, 'version': MOCK_RELEASE,
                           'current': self.version, 'new': True})
        if path == '/update/download':
            return self._job_start('download', {}, J)
        if path == '/update/apply':
            slot, err = self._slot_target(form, J)
            if err:
                return err
            f = form.get('file', '')
            if f not in ('download', 'upload'):
                return J(400, {'error': 'file must be download or upload'})
            if not self.slots['staged'][f]['present']:
                return J(404, {'error': 'staged archive not found'})
            return self._job_start('apply', {'slot': slot, 'file': f}, J)
        if path == '/update/upload':
            if self.update['running']:
                return J(409, {'error': 'an update job is running'})
            self.slots['staged']['upload'] = {
                'present': True, 'bytes': len(body), 'version': MOCK_RELEASE}
            return J(200, {'ok': True, 'file': 'upload', 'bytes': len(body),
                           'version': MOCK_RELEASE, 'signature': 'forgefirm'})
        if path == '/restore/factory':
            slot, err = self._slot_target(form, J)
            if err:
                return err
            f = form.get('file', '')
            if not f.startswith('factory-rootfs-'):
                return J(400, {'error': 'file must be a factory-rootfs '
                                        'archive name'})
            arch = [a for a in self.slots['archives'] if a['file'] == f]
            if not arch:
                return J(404, {'error': 'archive not found'})
            return self._job_start('restore',
                                   {'slot': slot, 'archive': arch[0]}, J)
        if path == '/logs/export':
            if not self.idle():
                return T(409, 'the machine is busy; export logs when it is '
                              'idle')
            full = form.get('sanitize', '1') in ('0', 'false', 'no')
            name = 'forgefirm-logs-%s%s.tar.gz' % (
                time.strftime('%Y%m%d-%H%M%S'), '-full' if full else '')
            return 200, {'Content-Type': 'application/gzip',
                         'Content-Disposition':
                         'attachment; filename="%s"' % name,
                         }, self._export_targz()
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

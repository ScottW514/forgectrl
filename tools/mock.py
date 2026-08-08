#!/usr/bin/env python3
"""Offline UI harness: serves the panel extracted from src/ui.c with
mock backend endpoints (settings/status/cam/diag), so the embedded
JavaScript can be exercised in a browser without a machine. POSTs to
/settings apply to the in-memory store and are logged to stdout -
that makes dirty-only saves and unit round-trips directly visible.

Usage: python3 tools/mock.py   then browse http://127.0.0.1:8081
"""
import json
import os
import re
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qsl

UI_C = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    '..', 'src', 'ui.c')
PORT = 8081


def extract_html():
    src = open(UI_C, encoding='utf-8').read()
    body = src[src.index('index_html[] ='):]
    body = body[:body.index('";\n') + 2]
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    html = ''.join(parts)
    return html.replace('\\"', '"').replace('\\\\', '\\')


HTML = extract_html()

settings = {
    'controller_mode': 'grbl', 'homing_mode': 'gfcloud',
    'gfcloud_home_x': '', 'gfcloud_home_y': '', 'gfcloud_home_z': '',
    'gfcloud_home_timeout_s': '', 'gf_serial': '', 'gf_password_set': False,
    'ui_units': '', 'cool_flow_rise': '', 'cool_flow_heater_pct': '',
    'cool_flow_check_s': '', 'cool_recheck_s': '', 'cool_confirm_max_s': '',
    'cool_temp_max': '', 'cool_temp_resume': '', 'cool_cooldown_s': '',
    'cool_cooldown_max_s': '',
    'version': 'mock', 'machine_id': 'ABC-123',
}

status = {
    'state': 'idle', 'homed': False, 'diag': False,
    'pos': {'x': 12.34, 'y': -5.6, 'z': 0.0},
    'laser_locked': True,
    'fans': {'air_assist': 1990, 'exhaust': 0, 'intake_1': 720,
             'intake_2': 730},
    'coolant': {'down_c': 22.4, 'up_c': 22.3, 'pump': True, 'tec': False},
    'switches': {'lid': True, 'button': False, 'interlock_ok': True,
                 'head': False, 'estop': False},
}

diag = {
    'running': False, 'tool': 'flow-calibrate', 'phase': 'done',
    'elapsed_s': 0, 'down_c': 24.1, 'up_c': 23.9,
    'log': ['  0:00  start: duty 40%, window 50 s, threshold 14.4 C',
            '  8:44  bands: flow <= 12.0, no-flow >= 17.6 (gap 5.7)'],
    'result': {'flow_rises': [11.6, 12.0, 12.0],
               'noflow_rises': [17.6, 17.7, 18.1],
               'flow_max': 12.0, 'noflow_min': 17.6, 'gap': 5.7,
               'recommend': 14.8},
}


class H(BaseHTTPRequestHandler):
    def _json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        p = urlparse(self.path)
        if p.path == '/':
            body = HTML.encode()
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif p.path == '/settings':
            self._json(settings)
        elif p.path == '/status':
            self._json(status)
        elif p.path == '/cam/status':
            self._json({'running': False, 'cam': 'lid', 'clients': 0,
                        'frames': 0, 'fps': 0, 'fps_cap': 0,
                        'encoder': 'vpu', 'buffers': 'cached'})
        elif p.path == '/diag/status':
            self._json(diag)
        elif p.path == '/fuse-identity':
            self._json({'serial': '123456789', 'hostname': 'ABC-123',
                        'password': '0123456789abcdef' * 4})
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        p = urlparse(self.path)
        if p.path == '/settings':
            q = dict(parse_qsl(p.query, keep_blank_values=True))
            print('POST /settings %s' % json.dumps(q), flush=True)
            for k, v in q.items():
                if k in settings:
                    settings[k] = v
            self._json(settings)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *a):
        pass


print('mock forgectrl on http://127.0.0.1:%d  (html %d bytes)'
      % (PORT, len(HTML)), flush=True)
HTTPServer(('127.0.0.1', PORT), H).serve_forever()

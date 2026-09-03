#!/usr/bin/env python3
"""Host test: the panel dev server's mock mirrors the daemon.

The mock in tools/devserver.py is a static copy of what forgectrl
serves. This test reads the daemon's own tables with a regex each (the
gate table in src/gates.c, the settings table and the endpoint list in
src/main.c, the format strings of the JSON builders, the routes the
panel calls in src/ui/*.js) and holds the mock to them, so a table
edit that misses the mock fails here instead of on a developer's
screen.

Run: python3 -B -m unittest -v tests/test_devserver_mock.py
"""
import importlib.util
import json
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding='utf-8') as f:
        return f.read()


def load_devserver():
    spec = importlib.util.spec_from_file_location(
        'devserver', os.path.join(ROOT, 'tools', 'devserver.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# -- the daemon's tables, read from the C sources
GATE_ROW = re.compile(
    r'\{\s*"(?P<key>\w+)",\s*(?:"(?P<gate>\w+)"|NULL),\s*'
    r'(?P<def>[-\d.]+),\s*(?P<lo>[-\d.]+),\s*(?P<hi>[-\d.]+),\s*'
    r'(?P<blo>[-\d.]+),\s*(?P<bhi>[-\d.]+),\s*(?P<off>[-+]?\d)\s*\}')
OFF_END = {'-1': 'low', '+1': 'high', '1': 'high', '0': 'none'}


def gate_table():
    """(key, gate, def, lo, hi, band_lo, band_hi, off) per row of the
    table in gates.c, in table order."""
    m = re.search(r'gate_setting_t table\[\] = \{(.*?)\n\};',
                  read('src/gates.c'), re.S)
    rows = []
    for r in GATE_ROW.finditer(m.group(1)):
        rows.append((r.group('key'), r.group('gate'),
                     float(r.group('def')), float(r.group('lo')),
                     float(r.group('hi')), float(r.group('blo')),
                     float(r.group('bhi')), OFF_END[r.group('off')]))
    return rows


SETTING_ROW = re.compile(
    r'\{\s*"(?P<key>\w+)",\s*\w+,\s*(?P<secret>[01])\s*\}')


def settings_table():
    """(key, secret) per row of setting_defs in main.c, in table order."""
    m = re.search(r'\} setting_defs\[\] = \{(.*?)\n\};', read('src/main.c'),
                  re.S)
    return [(r.group('key'), r.group('secret') == '1')
            for r in SETTING_ROW.finditer(m.group(1))]


def endpoints():
    """(method, path) per ulfius_add_endpoint_by_val call in main.c."""
    return re.findall(r'ulfius_add_endpoint_by_val\(&inst, "(GET|POST)", '
                      r'"([^"]+)"', read('src/main.c'))


def panel_routes():
    """The API paths the panel's JavaScript calls (a quoted literal that
    starts with a slash, at a fetch/fx/startJob call or as a stream
    source), less the diag prefix it completes at run time."""
    paths = set()
    for name in os.listdir(os.path.join(ROOT, 'src', 'ui')):
        if not name.endswith('.js'):
            continue
        js = read(os.path.join('src', 'ui', name))
        for p in re.findall(r"(?:fetch|fx|startJob)\('(/[^'?]*)", js):
            paths.add(p)
        for p in re.findall(r"src = '(/[^'?]*)", js):
            paths.add(p)
    return sorted(p for p in paths if p != '/' and not p.endswith('/'))


def c_keys(src, func=None):
    """Every JSON key a C source (or one function of it) writes: the
    \\"name\\": tokens of its format strings."""
    if func:
        m = re.search(r'^[\w \*]*\b%s\(' % re.escape(func), src, re.M)
        end = src.index('\n}\n', m.start())
        src = src[m.start():end]
    return set(re.findall(r'\\"(\w+)\\":', src))


def doc_keys(obj):
    """Every key at any depth of a JSON document."""
    out = set()
    if isinstance(obj, dict):
        for k, v in obj.items():
            out.add(k)
            out |= doc_keys(v)
    elif isinstance(obj, list):
        for v in obj:
            out |= doc_keys(v)
    return out


class MockTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ds = load_devserver()
        cls.token = cls.ds.MOCK_TOKEN

    def mock(self):
        return self.ds.Mock(self.token)

    def call(self, mock, method, path, q=None, body=b'', token=True):
        headers = {'Content-Type': 'application/x-www-form-urlencoded'}
        if token:
            headers['X-ForgeFIRM-Token'] = self.token
        return mock.handle(method, path, q or {}, headers, body)

    def get_json(self, mock, path, q=None):
        code, hdrs, body = self.call(mock, 'GET', path, q)
        self.assertEqual(code, 200, path)
        self.assertEqual(hdrs['Content-Type'], 'application/json', path)
        return json.loads(body)

    # -- the gate table
    def test_gate_table_matches_gates_c(self):
        table = gate_table()
        self.assertGreater(len(table), 0, 'gates.c table parsed')
        self.assertEqual(list(self.ds.Mock.GATES), table)

    def test_gate_state_rule(self):
        for row in self.ds.Mock.GATES:
            key, gate, default, lo, hi, blo, bhi, off = row
            st = self.ds.Mock.gate_state
            self.assertEqual(st(row, default), 'ok', key)
            if off == 'low':
                self.assertEqual(st(row, lo), 'off', key)
            elif off == 'high':
                self.assertEqual(st(row, hi), 'off', key)
            if blo > lo:
                self.assertEqual(st(row, blo - 0.001), 'warn', key)
            if bhi < hi:
                self.assertEqual(st(row, bhi + 0.001), 'warn', key)

    def test_settings_gates_block(self):
        m = self.mock()
        gates = self.get_json(m, '/settings')['gates']
        self.assertEqual(list(gates), [r[0] for r in self.ds.Mock.GATES])
        keys = c_keys(read('src/gates.c'), 'gates_json')
        for key, g in gates.items():
            self.assertEqual(set(g), keys, key)
        # A value at the off end is reported off, and lands in gates_off
        # on /status and /cool/status the way the engine reports it.
        m.settings['cool_flow_check_s'] = '0'
        self.assertEqual(self.get_json(m, '/settings')['gates']
                         ['cool_flow_check_s']['state'], 'off')
        self.assertIn('flow', self.get_json(m, '/status')['gates_off'])
        self.assertIn('flow', self.get_json(m, '/cool/status')['gates_off'])

    # -- the settings key set
    def test_settings_keys_match_main_c(self):
        table = settings_table()
        self.assertGreater(len(table), 0, 'main.c setting_defs parsed')
        self.assertEqual(list(self.ds.SETTINGS_KEYS), [k for k, _ in table])
        self.assertEqual(list(self.ds.SECRET_KEYS),
                         [k for k, secret in table if secret])
        expect = [k + '_set' if secret else k for k, secret in table]
        expect += ['gates', 'version', 'machine_id']
        reply = self.get_json(self.mock(), '/settings')
        self.assertEqual(list(reply), expect)
        for k, secret in table:
            self.assertIsInstance(reply[k + '_set' if secret else k],
                                  bool if secret else str, k)

    def test_settings_post(self):
        m = self.mock()
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cool_temp_max': '35'})
        self.assertEqual(code, 200)
        self.assertEqual(json.loads(body)['cool_temp_max'], '35')
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cool_temp_max': '999'})
        self.assertEqual((code, hdrs['Content-Type'], body),
                         (400, 'text/plain',
                          b'invalid value for cool_temp_max'))
        code, hdrs, body = self.call(m, 'POST', '/settings', {'bogus': '1'})
        self.assertEqual((code, body), (400, b'no known setting in request'))
        code, hdrs, body = self.call(m, 'POST', '/settings',
                                     {'cool_temp_max': '35'}, token=False)
        self.assertEqual(code, 403)
        self.assertEqual(json.loads(body),
                         {'error': 'authentication required'})

    # -- the mode vocabulary
    def test_mode_matches_super_c(self):
        src = read('src/super.c')
        switch = re.search(r'int super_mode_switch\(.*?\n\}', src, re.S)
        accepted = re.findall(r'!strcmp\(mode, "(\w+)"\)', switch.group(0))
        self.assertEqual(sorted(accepted), ['cloud', 'grbl'])
        m = self.mock()
        doc = self.get_json(m, '/mode')
        self.assertEqual(set(doc), c_keys(src, 'super_status_json'))
        states = set(re.findall(r'"(running|motion-fault|standby|stopped)"',
                                src))
        self.assertIn(doc['controller'], states)
        for mode in accepted:
            code, hdrs, body = self.call(m, 'POST', '/mode',
                                         {'controller': mode})
            self.assertEqual(code, 200, mode)
            self.assertEqual(json.loads(body)['mode'], mode)
            self.assertEqual(self.get_json(m, '/settings')['controller_mode'],
                             mode)
        code, hdrs, body = self.call(m, 'POST', '/mode',
                                     {'controller': 'gfcloud'})
        self.assertEqual((code, body), (400, b'mode must be grbl or cloud'))
        code, hdrs, body = self.call(m, 'POST', '/mode')
        self.assertEqual((code, body), (400, b'controller is required'))

    # -- the document shapes, against the C format strings
    def test_cool_status_shape(self):
        src = read('src/coolfmt.c')
        keys = (c_keys(src, 'coolfmt_status') | c_keys(src, 'coolfmt_limits')
                | c_keys(src, 'coolfmt_fan_gates'))
        fans = re.search(r'fan_name\[Fan_N\] = \{([^}]*)\}',
                         read('src/cool.c')).group(1)
        keys |= set(re.findall(r'"(\w+)"', fans))
        doc = self.get_json(self.mock(), '/cool/status')
        self.assertEqual(doc_keys(doc), keys)
        self.assertEqual(set(doc), c_keys(src, 'coolfmt_status'))
        self.assertEqual(list(doc['fan_gates']),
                         re.findall(r'"(\w+)"', fans))

    def test_status_shape(self):
        keys = c_keys(read('src/status.c')) | {'gates_off'}
        doc = self.get_json(self.mock(), '/status')
        self.assertTrue(keys <= doc_keys(doc), keys - doc_keys(doc))
        self.assertTrue(set(doc) <= keys, set(doc) - keys)
        report = doc['grbl']['report']
        ctl = os.path.join(ROOT, '..', 'grblHAL-glowforge', 'src',
                           'glowforge_status.c')
        if os.path.isfile(ctl):
            with open(ctl, encoding='utf-8') as f:
                self.assertEqual(doc_keys(report), c_keys(f.read()))

    def test_diag_shape_and_tools(self):
        src = read('src/diag.c')
        m = self.mock()
        doc = self.get_json(m, '/diag/status')
        self.assertEqual(set(doc), c_keys(src, 'diag_status_json'))
        tools = set(re.findall(r'!strcmp\(tool, "([\w-]+)"\)', src))
        self.assertEqual(set(self.ds.DIAG_TOOLS), tools)
        for tool in tools:
            code, hdrs, body = self.call(m, 'POST', '/diag/' + tool)
            self.assertEqual((code, json.loads(body)),
                             (202, {'started': True}), tool)
            self.assertTrue(self.get_json(m, '/status')['diag'])
            code, hdrs, body = self.call(m, 'POST', '/diag/' + tool)
            self.assertEqual((code, body),
                             (409, b'a diagnostic is already running'))
            code, hdrs, body = self.call(m, 'POST', '/diag/abort')
            self.assertEqual(json.loads(body), {'aborting': True})
            doc = self.get_json(m, '/diag/status')
            self.assertFalse(doc['running'])
            self.assertEqual(doc['result'], {'error': 'aborted by operator'})
        code, hdrs, body = self.call(m, 'POST', '/diag/bogus')
        self.assertEqual((code, body), (400, b'unknown diagnostic'))
        for tool, result in self.ds.DIAG_RESULTS.items():
            self.assertIn(tool, tools)
            self.assertTrue(doc_keys(result) <= c_keys(src),
                            doc_keys(result) - c_keys(src))

    def test_cam_status_shape(self):
        keys = c_keys(read('src/main.c'), 'cb_status')
        self.assertEqual(doc_keys(self.get_json(self.mock(), '/cam/status')),
                         keys)

    def test_update_shapes(self):
        src = read('src/update.c')
        m = self.mock()
        self.assertTrue(c_keys(src, 'cb_slots') <=
                        doc_keys(self.get_json(m, '/slots')))
        self.assertEqual(set(self.get_json(m, '/update/status')),
                         c_keys(src, 'cb_update_status'))
        code, hdrs, body = self.call(m, 'POST', '/update/check')
        self.assertTrue(set(json.loads(body)) <=
                        c_keys(src, 'cb_update_check'))
        code, hdrs, body = self.call(m, 'POST', '/boot', {'target': 'c'})
        self.assertEqual(json.loads(body),
                         {'error': 'target must be sd, a, b, or legacy'})
        code, hdrs, body = self.call(m, 'POST', '/system/reboot')
        self.assertEqual((code, json.loads(body)),
                         (400, {'error': 'confirm=1 required'}))
        code, hdrs, body = self.call(m, 'POST', '/update/download')
        self.assertEqual((code, json.loads(body)), (202, {'started': True}))
        code, hdrs, body = self.call(m, 'POST', '/update/download')
        self.assertEqual((code, json.loads(body)),
                         (409, {'error': 'an update job is already running'}))
        code, hdrs, body = self.call(m, 'POST', '/boot', {'target': 'b'})
        self.assertEqual((code, json.loads(body)),
                         (409, {'error': 'an update job is running'}))

    def test_logs_shapes(self):
        src = read('src/logs.c')
        m = self.mock()
        code, hdrs, body = self.call(m, 'GET', '/logs', token=False)
        self.assertEqual(code, 403)
        self.assertEqual(doc_keys(self.get_json(m, '/logs')),
                         c_keys(src, 'logs_list_json'))
        self.assertEqual(set(self.get_json(m, '/logs/tail',
                                           {'name': 'forgectrl'})),
                         c_keys(src, 'logs_tail_json'))
        self.assertEqual(self.ds.LOGGERS,
                         tuple(re.findall(r'"(\w+)"', re.search(
                             r'logs_names\[\] = \{([^}]*)\}', src).group(1))))
        self.assertEqual(self.ds.LOG_LEVELS,
                         tuple(re.findall(r'"(\w+)"', re.search(
                             r'level_names\[\] = \{([^}]*)\}', src).group(1))))

    def test_curve_shape(self):
        keys = c_keys(read('src/curverec.c'), 'curverec_status_json')
        m = self.mock()
        self.assertTrue(set(self.get_json(m, '/curve/status')) <= keys)
        code, hdrs, body = self.call(m, 'POST', '/curve/record')
        self.assertEqual(code, 200)
        self.assertEqual(doc_keys(json.loads(body)) - keys, set())
        code, hdrs, body = self.call(m, 'POST', '/curve/stop')
        self.assertEqual(doc_keys(json.loads(body)),
                         keys - {'density', 'light'})

    # -- every route the daemon registers, and every route the panel calls
    def test_every_daemon_route_is_served(self):
        m = self.mock()
        missing = []
        for method, path in endpoints():
            if path == '/':
                continue            # the page: the dev server's own
            code, hdrs, body = self.call(m, method, path)
            if (code == 404 and b'mock: no such endpoint' in body) or \
                    code == 405:
                missing.append('%s %s' % (method, path))
        self.assertEqual(missing, [])

    def test_every_panel_route_is_registered(self):
        registered = {p for _, p in endpoints()}
        routes = panel_routes()
        self.assertGreater(len(routes), 10, 'panel routes parsed')
        self.assertEqual([p for p in routes if p not in registered], [])


if __name__ == '__main__':
    unittest.main()

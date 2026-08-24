/*
 * panel.js - forgectrl: the control panel
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Single-page machine control panel, hash-routed tabs, built on Bootstrap
 * (vendor/) with the OpenGlow theme on top (theme.css):
 *
 *   #status   landing page - live operational status (motion state and
 *             position, cooling and fans, safety switches, system
 *             summary) plus a scaled lid snapshot that switches to the
 *             live MJPEG stream on demand
 *   #machine  shared machine settings (display units, homing method +
 *             calibration, cooling tunables)
 *   #gfcloud  Glowforge cloud overrides (identity, homing session)
 *             plus the fuse-identity viewer (modal, on demand)
 *   #grbl     GRBL-mode settings (controller info; tunables land here
 *             as the driver exposes them)
 *   #diag     diagnostics - tools that take the hardware over (the
 *             motion controller is stopped for the duration): cooling
 *             system verification and calibration
 *   #logs     logging - per-logger disk and remote levels (applied at
 *             reboot), remote syslog target, live log viewer, and the
 *             sanitized tar.gz export for issue reports
 *   #system   firmware slots (A/B boot selection), ForgeFIRM updates,
 *             image install/restore, wireless regulatory region, reboot
 *
 * State flows through GET/POST /settings, GET /status (machine
 * telemetry), GET /cam/status and GET /logs. The design intent: every
 * machine tunable - shared, cloud-override, and GRBL-mode - gets a home
 * in one of these tabs as it appears. Every settings field on every tab
 * feeds one dirty set and one Save (forms.js): the save bar shows while
 * anything is unsaved, posts every dirty key in one request, and leaving
 * a tab or the page with unsaved changes asks first. Help is a "?"
 * popover per card or field (help.js), each linking into the
 * documentation site; the light and dark themes are picked in the
 * header. Refills from the daemon (after a save, a mode switch, a
 * diagnostic's Apply) never overwrite a field the operator is editing:
 * setF keeps a dirty value and only moves its baseline.
 *
 * The header identifies the machine by its fuse identity (the factory
 * hostname derived from the burned-in serial) regardless of any cloud
 * identity override. Units are a display-only preference (ui_units):
 * the backend stores metric, conversion happens at the UI edge, and
 * saves post only fields whose display string actually changed.
 *
 * TOK is the panel bearer token: the daemon replaces the placeholder in
 * its initializer (the only occurrence in the page) when it serves the
 * page, and every state-changing request carries it in X-ForgeFIRM-Token
 * (fx below). A cross-origin page cannot read it, so it cannot forge one.
 */
var S = {},
  CS = {},
  M = {},
  D = {},
  liveOn = false,
  retries = 0,
  locked = false;
var TOK = '__FFTOKEN__';
var CK = [
  'cool_flow_rise',
  'cool_flow_heater_pct',
  'cool_flow_check_s',
  'cool_recheck_s',
  'cool_confirm_max_s',
  'cool_temp_max',
  'cool_temp_resume',
  'cool_temp_critical_c',
  'cool_cooldown_s',
  'cool_cooldown_max_s',
  'cool_tach_exhaust_min_rpm',
  'cool_tach_intake_min_rpm',
  'cool_tach_air_assist_min_rpm',
  'cool_purge_min_current',
  'cool_fan_grace_s'
];
function fx(u, o) {
  o = o || {};
  o.headers = o.headers || {};
  o.headers['X-ForgeFIRM-Token'] = TOK;
  return fetch(u, o);
}
function isImp() {
  return (S.ui_units || 'metric') === 'imperial';
}
function uL() {
  return isImp() ? 'in' : 'mm';
}
function uT() {
  return isImp() ? '\u00b0F' : '\u00b0C';
}
function fnum(x, dp) {
  return String(parseFloat(x.toFixed(dp)));
}
function dLen(v) {
  return isImp() ? v / 25.4 : v;
}
function dTa(v) {
  return isImp() ? v * 1.8 + 32 : v;
}
function dTd(v) {
  return isImp() ? v * 1.8 : v;
}
function pLen(v) {
  return isImp() ? v * 25.4 : v;
}
function pTa(v) {
  return isImp() ? (v - 32) / 1.8 : v;
}
function pTd(v) {
  return isImp() ? v / 1.8 : v;
}
var FT = {
  gfcloud_home_x: 'len',
  gfcloud_home_y: 'len',
  gfcloud_home_z: 'len',
  cool_flow_rise: 'td',
  cool_temp_max: 'ta',
  cool_temp_resume: 'ta',
  cool_temp_critical_c: 'ta'
};
var PH = {
  cool_flow_rise: [14.4, 'td'],
  cool_temp_max: [33, 'ta'],
  cool_temp_resume: [31, 'ta'],
  cool_temp_critical_c: [38, 'ta']
};
var orig = {};
/* The gate settings (S.gates, from /settings): each carries its legal
 * range, recommended band, off end and the state of the stored value.
 * The notes under the cooling card re-classify the typed value live, in
 * the panel's units; the Status banner reports any gate that is off. */
var GN = {
  cool_temp_max: 'Coolant ceiling',
  cool_temp_resume: 'Resume gate',
  cool_temp_critical_c: 'Coolant critical',
  cool_flow_check_s: 'Flow check window',
  cool_flow_rise: 'Flow fault rise',
  cool_tach_exhaust_min_rpm: 'Exhaust floor',
  cool_tach_intake_min_rpm: 'Intake floor',
  cool_tach_air_assist_min_rpm: 'Air assist floor',
  cool_purge_min_current: 'Purge current floor',
  cool_fan_grace_s: 'Spin-up grace'
};
var GG = {
  coolant_max: 'the coolant ceiling',
  flow: 'coolant flow verification',
  exhaust: 'the exhaust fan',
  intake: 'the intake fans',
  air_assist: 'the air assist',
  purge: 'the purge fan'
};
var GU = {
  cool_tach_exhaust_min_rpm: 'rpm',
  cool_tach_intake_min_rpm: 'rpm',
  cool_tach_air_assist_min_rpm: 'rpm',
  cool_purge_min_current: 'raw'
};
function gUnit(k) {
  return FT[k] ? uT() : GU[k] || 's';
}
function gDsp(k, v) {
  return FT[k] ? fnum(FT[k] === 'ta' ? dTa(v) : dTd(v), 1) : String(v);
}
function gState(g, v) {
  if (g.off === 'low' && v <= g.lo) return 'off';
  if (g.off === 'high' && v >= g.hi) return 'off';
  if (v < g.band[0] || v > g.band[1]) return 'warn';
  return 'ok';
}
function gValue(k, g) {
  var t = $(k) ? $(k).value.trim() : '';
  var v = t === '' ? g.def : parseFloat(backVal(k, t));
  return isNaN(v) ? g.def : v;
}
function renderGateNotes() {
  var el = $('gnotes'),
    g = S.gates,
    k,
    h = '';
  if (!el || !g) return;
  for (k in GN) {
    if (!g[k]) continue;
    var v = gValue(k, g[k]),
      st = gState(g[k], v),
      band = gDsp(k, g[k].band[0]) + ' to ' + gDsp(k, g[k].band[1]) + ' ' + gUnit(k);
    if (st === 'off')
      h +=
        "<div class='gn-off'>\u26A0 " +
        GN[k] +
        ' ' +
        gDsp(k, v) +
        ' ' +
        gUnit(k) +
        ': this gate is OFF (recommended ' +
        band +
        ')</div>';
    else if (st === 'warn')
      h +=
        "<div class='gn-warn'>\u26A0 " +
        GN[k] +
        ' ' +
        gDsp(k, v) +
        ' ' +
        gUnit(k) +
        ' is outside the recommended ' +
        band +
        '</div>';
  }
  el.innerHTML = h;
}
function renderGatesBanner() {
  var b = $('gates-banner'),
    g = S.gates,
    k,
    off = [];
  if (!b) return;
  if (g)
    for (k in g) if (g[k].gate && g[k].state === 'off') off.push(g[k].gate);
  if (!off.length) {
    b.style.display = 'none';
    return;
  }
  var names = [];
  for (k = 0; k < off.length; k++) names.push(GG[off[k]] || off[k]);
  b.innerHTML =
    '\u26A0 Cooling gate off: ' +
    esc(names.join(', ')) +
    '. The laser will fire without this protection' +
    (off.indexOf('flow') >= 0 || off.indexOf('coolant_max') >= 0
      ? ', so a stopped pump or an overheating loop will not hold a job'
      : '') +
    (off.indexOf('exhaust') >= 0 || off.indexOf('intake') >= 0 ||
    off.indexOf('air_assist') >= 0 || off.indexOf('purge') >= 0
      ? ', so a stalled fan will not hold a job'
      : '') +
    ". Set the value back inside its band on the <a href='#machine'>Machine tab</a>.";
  b.style.display = '';
}
function dspVal(k, raw) {
  var f = parseFloat(raw);
  if (raw === '' || isNaN(f) || !FT[k]) return raw;
  return FT[k] === 'len' ? fnum(dLen(f), 3) : fnum(FT[k] === 'ta' ? dTa(f) : dTd(f), 1);
}
function backVal(k, s) {
  var f = parseFloat(s);
  if (s === '' || isNaN(f) || !FT[k]) return s;
  return FT[k] === 'len' ? fnum(pLen(f), 3) : fnum(FT[k] === 'ta' ? pTa(f) : pTd(f), 2);
}
/* A refill only replaces a field that is clean (showing its baseline);
 * a value the operator has typed stays, with the baseline moved under
 * it. fill(true) after a save resets everything to what the daemon
 * reports. */
var fillForce = false;
function setF(k, raw) {
  var v = dspVal(k, raw || ''),
    e = $(k),
    clean = typeof orig[k] === 'undefined' || e.value.trim() === orig[k];
  if (fillForce || clean) e.value = v;
  orig[k] = v;
}
function dirty(k) {
  return typeof orig[k] !== 'undefined' && $(k).value.trim() !== orig[k];
}
function lockApply() {
  var els = document.querySelectorAll('main input,main select,main button');
  for (var i = 0; i < els.length; i++) {
    var e = els[i];
    if (e.getAttribute('data-nolock')) continue;
    e.disabled = locked || e.getAttribute('data-off') === '1';
  }
  $('locknote').style.display = locked ? 'block' : 'none';
  if (locked)
    $('locknote').textContent = D.running
      ? 'A diagnostic is running: controls are locked until it completes.'
      : 'Settings are locked while the machine is busy (state: ' +
        (M.state || 'unknown') +
        ').';
  updateSaveBar();
}
function setMode(m) {
  var el = $('msg-mode');
  el.textContent = 'switching\u2026';
  fx('/mode?controller=' + m, { method: 'POST' })
    .then(function (r) {
      return r.text().then(function (t) {
        if (r.ok) {
          el.textContent = 'running ' + m;
          setTimeout(function () {
            el.textContent = '';
          }, 4000);
          return fetch('/settings')
            .then(function (r2) {
              return r2.json();
            })
            .then(function (s) {
              S = s;
              fill();
            });
        }
        el.textContent = t;
      });
    })
    .catch(function () {
      el.textContent = 'no response';
    });
}
var TABS = ['status', 'machine', 'gfcloud', 'grbl', 'diag', 'logs', 'system'],
  sysChecked = false,
  curTab = null,
  hashRevert = false;
function tabOf(hash) {
  var h = hash.replace('#', '');
  return TABS.indexOf(h) < 0 ? 'status' : h;
}
/* The hash router. Leaving a tab with unsaved changes puts the hash
 * back and asks (navGuard, forms.js); the answer re-issues the move. */
function onHash() {
  if (hashRevert) {
    hashRevert = false;
    return;
  }
  var h = tabOf(location.hash);
  if (curTab && h !== curTab && dirtyCount()) {
    hashRevert = true;
    location.hash = '#' + curTab;
    navGuard(function () {
      location.hash = '#' + h;
    });
    return;
  }
  tab();
}
function tab() {
  var h = tabOf(location.hash);
  curTab = h;
  for (var i = 0; i < TABS.length; i++) {
    var t = TABS[i];
    $('s-' + t).className = t === h ? 'on' : '';
    $('t-' + t).className = t === h ? 'on' : '';
  }
  if (h === 'system' && !sysChecked) {
    sysChecked = true;
    checkUpd();
  }
  if (h === 'logs') {
    loadLogs();
    loadTail(false);
  }
}
window.onhashchange = onHash;
function snapUrl() {
  return '/cam/snapshot?cam=lid&res=half&t=' + Date.now();
}
function refreshSnap() {
  if (!liveOn) $('cam').src = snapUrl();
}
// Live view: H.264 over MSE when the browser and the machine both offer
// it (a fraction of MJPEG's bytes, which matters on the machine's WiFi),
// MJPEG otherwise. Any H.264 setup or mid-stream failure falls back to
// MJPEG for the rest of the session.
var h264Abort = null;
var h264Failed = false;

function stopH264() {
  if (h264Abort) {
    h264Abort.abort();
    h264Abort = null;
  }
  var v = $('camvid');
  v.style.display = 'none';
  v.removeAttribute('src');
  $('cam').style.display = '';
}

function startMjpeg() {
  stopH264();
  $('cam').src = '/cam/stream?cam=lid&t=' + Date.now();
}

function startH264() {
  if (h264Failed || !window.MediaSource) {
    startMjpeg();
    return;
  }
  h264Abort = new AbortController();
  var signal = h264Abort.signal;
  fetch('/cam/h264?cam=lid', { signal: signal })
    .then(function (resp) {
      if (!resp.ok) throw new Error('h264 ' + resp.status);
      var codec = resp.headers.get('X-H264-Codec') || 'avc1.42e01e';
      var mime = 'video/mp4; codecs="' + codec + '"';
      if (!MediaSource.isTypeSupported(mime)) throw new Error(mime);
      var ms = new MediaSource();
      var v = $('camvid');
      v.src = URL.createObjectURL(ms);
      var reader = resp.body.getReader();
      ms.addEventListener('sourceopen', function () {
        var sb = ms.addSourceBuffer(mime);
        var queue = [];
        function pumpQueue() {
          if (queue.length && !sb.updating) sb.appendBuffer(queue.shift());
        }
        sb.addEventListener('updateend', function () {
          // Stay at the live edge, clamped inside the newest buffered
          // range: early on only a frame or two is buffered, and a
          // fixed back-off would land in a gap and stall.
          if (v.buffered.length) {
            var last = v.buffered.length - 1;
            var start = v.buffered.start(last);
            var end = v.buffered.end(last);
            if (v.currentTime < start || end - v.currentTime > 1.5)
              v.currentTime = Math.max(start, end - 0.3);
            if (end - v.buffered.start(0) > 30 && !sb.updating)
              sb.remove(v.buffered.start(0), end - 10);
          }
          // A seek near the live edge can leave the element paused.
          if (v.paused) v.play().catch(function () {});
          pumpQueue();
        });
        (function read() {
          reader
            .read()
            .then(function (r) {
              if (r.done) {
                // Ended upstream: the lid opened or another viewer took
                // the camera. Freeze on the last frame and say so.
                if (liveOn && !signal.aborted)
                  $('cammsg').textContent = 'stream ended';
                return;
              }
              if (!liveOn) return;
              queue.push(r.value);
              pumpQueue();
              read();
            })
            .catch(function () {
              if (liveOn && !signal.aborted) {
                h264Failed = true;
                startMjpeg();
              }
            });
        })();
      });
      v.style.display = '';
      $('cam').style.display = 'none';
      v.play().catch(function () {});
    })
    .catch(function () {
      if (liveOn && !signal.aborted) {
        h264Failed = true;
        startMjpeg();
      }
    });
}

function toggleLive() {
  liveOn = !liveOn;
  retries = 0;
  $('cammsg').textContent = '';
  $('live').textContent = liveOn ? 'Stop' : 'Live';
  $('live').classList.toggle('on', liveOn);
  $('snapbtn').disabled = liveOn;
  if (liveOn) {
    startH264();
  } else {
    stopH264();
    $('cam').src = snapUrl();
  }
}
$('cam').onerror = function () {
  if (!liveOn) return;
  fetch('/cam/status')
    .then(function (r) {
      return r.json();
    })
    .then(function (s) {
      // An open lid is the privacy gate, not a fault: say so and stop
      // retrying, because retrying cannot succeed until the lid closes.
      if (s.capture_allowed === false) {
        $('cammsg').textContent = 'lid open \u2014 the cameras are off';
        toggleLive();
        return;
      }
      if (s.cam === 'lid' && retries++ < 5) {
        $('cammsg').textContent = 'stream retrying\u2026';
        setTimeout(function () {
          if (liveOn) $('cam').src = '/cam/stream?cam=lid&t=' + Date.now();
        }, 700);
      } else {
        $('cammsg').textContent =
          s.cam !== 'lid' ? 'stream taken by another viewer (' + s.cam + ')' : 'stream error';
        toggleLive();
      }
    })
    .catch(function () {
      $('cammsg').textContent = 'service unreachable';
    });
};
function kv(k, v, cls) {
  return (
    "<div class='kv'><span>" +
    esc(k) +
    '</span><span' +
    (cls ? " class='" + cls + "'" : '') +
    '>' +
    v +
    '</span></div>'
  );
}
function txt(k, v, cls) {
  return kv(k, esc(v), cls);
}
function modeName(m) {
  return m === 'gfcloud'
    ? 'Glowforge web service'
    : m === 'switches'
      ? 'Limit switches'
      : 'Disabled';
}
function stateBadge(st) {
  var c =
    st === 'idle' ? 'b-ok' : st === 'running' ? 'b-run' : st === 'disabled' ? 'b-dim' : 'b-bad';
  return txt('State', st, c);
}
function posN(v) {
  return dLen(v).toFixed(isImp() ? 3 : 2);
}
function renderMotion() {
  var g = '';
  g += stateBadge(M.state || 'unknown');
  if (M.pos)
    g += kv(
      'Position',
      "<span class='mono" +
        (M.homed ? '' : ' b-bad') +
        "'>X " +
        posN(M.pos.x) +
        ' &nbsp;Y ' +
        posN(M.pos.y) +
        ' &nbsp;Z ' +
        posN(M.pos.z) +
        ' ' +
        uL() +
        '</span>'
    );
  g += txt('Homed', M.homed ? 'yes' : 'no', M.homed ? 'b-ok' : 'b-dim');
  if (typeof M.laser_locked !== 'undefined')
    g += txt(
      'Laser latch (commanded)',
      M.laser_locked ? 'locked' : 'UNLOCKED',
      M.laser_locked ? 'b-ok' : 'b-bad'
    );
  if (M.laser) {
    g += txt(
      'Laser emission (sensed)',
      M.laser.emission_samples > 0 ? 'EMITTING' : 'dark',
      M.laser.emission_samples > 0 ? 'b-run' : 'b-ok'
    );
  }
  if (typeof M.faults !== 'undefined' && M.faults > 0)
    g += txt('Stepper faults', 'mask ' + M.faults, 'b-bad');
  if (M.lid_ir) g += kv('Lid IR', "<span class='mono'>" + M.lid_ir.join(' / ') + '</span>');
  if (typeof M.hv_current_raw !== 'undefined')
    g += kv('HV current', "<span class='mono'>" + M.hv_current_raw + ' raw</span>');
  $('motion').innerHTML = g;
}
function rpm(v) {
  return v > 0 ? v + ' rpm' : 'stopped';
}
function degc(v) {
  return v > -100 ? dTa(v).toFixed(1) + ' ' + uT() : '\u2014';
}
function renderCooling() {
  if (!M.coolant) return;
  var g = '';
  g += txt('Coolant out', degc(M.coolant.down_c));
  g += txt('Coolant in', degc(M.coolant.up_c));
  g += txt('Pump', M.coolant.pump ? 'on' : 'off', M.coolant.pump ? 'b-ok' : 'b-warn');
  g += txt('TEC', M.coolant.tec ? 'on' : 'off', 'b-dim');
  if (M.temps) {
    g += txt('Chassis', M.temps.chassis_c == null ? '\u2014' : degc(M.temps.chassis_c));
    g += txt('SoC die', M.temps.soc_c == null ? '\u2014' : degc(M.temps.soc_c),
             M.temps.soc_c != null && M.temps.soc_c >= 80 ? 'b-warn' : '');
    var thr = M.temps.soc_throttle;
    g += txt('SoC throttle', thr == null ? '\u2014' : (thr > 0 ? 'state ' + thr : 'none'),
             thr > 0 ? 'b-warn' : 'b-dim');
    g += txt('Supply (raw)', M.temps.supply_raw == null ? '\u2014' : String(M.temps.supply_raw), 'b-dim');
  }
  if (M.sys) {
    g += txt('CPU', M.sys.cpu_pct == null ? '\u2014' : M.sys.cpu_pct.toFixed(0) + '%',
             M.sys.cpu_pct != null && M.sys.cpu_pct >= 90 ? 'b-warn' : '');
    g += txt('Memory', M.sys.mem_pct == null ? '\u2014' : M.sys.mem_pct.toFixed(0) + '%',
             M.sys.mem_pct != null && M.sys.mem_pct >= 90 ? 'b-warn' : '');
  }
  if (M.fans) {
    g += txt('Air assist', rpm(M.fans.air_assist));
    g += txt('Exhaust', rpm(M.fans.exhaust));
    g += txt('Intake 1', rpm(M.fans.intake_1));
    g += txt('Intake 2', rpm(M.fans.intake_2));
  }
  $('cooling').innerHTML = g;
}
function sw(k, name, on, off, onCls, offCls) {
  var v = M.switches[k];
  return txt(name, v ? on : off, v ? onCls : offCls);
}
function renderSwitches() {
  if (!M.switches) return;
  var g = '';
  g += sw('lid', 'Lid', 'closed', 'OPEN', 'b-ok', 'b-warn');
  g += sw('button', 'Button', 'pressed', 'released', 'b-run', 'b-dim');
  g += sw('interlock_ok', 'Remote interlock', 'satisfied', 'OPEN (lockout)', 'b-ok', 'b-warn');
  g += sw('head', 'Head sense', 'detected', 'not detected', 'b-ok', 'b-dim');
  g += sw('hv_enable', 'HV enable', 'on', 'off', 'b-run', 'b-dim');
  $('switches').innerHTML = g;
}
function renderStat() {
  var g = '';
  g += txt('Firmware', S.version || '\u2014');
  g += txt('Homing', modeName(S.homing_mode));
  g += txt('Identity', S.gf_serial ? 'Override (S' + S.gf_serial + ')' : 'Factory fuses');
  g += kv(
    'Controller',
    "<span class='mono'>" + esc(location.hostname) + ':23</span> (Grbl 1.1)'
  );
  var sensor = CS.sensor && CS.sensor !== 'unknown' ? CS.sensor + ', ' : '';
  if (CS.capture_allowed === false)
    g += txt('Camera engine', sensor + 'off — lid open (privacy)');
  else if (CS.running)
    g += txt(
      'Camera engine',
      sensor +
        CS.cam +
        ' camera, ' +
        (CS.fps || 0).toFixed(1) +
        ' fps, ' +
        CS.encoder +
        ', ' +
        CS.clients +
        ' viewer' +
        (CS.clients === 1 ? '' : 's')
    );
  else g += txt('Camera engine', sensor ? sensor + 'idle' : 'idle');
  $('statgrid').innerHTML = g;
}
function renderGrbl() {
  var g = '';
  g += txt('Protocol', 'Grbl 1.1 (grblHAL)');
  g += kv('Connection', "<span class='mono'>" + esc(location.hostname) + ':23</span>');
  g += kv(
    'Camera stream',
    "<span class='mono'>http://" + esc(location.host) + '/?action=stream</span>'
  );
  $('grblinfo').innerHTML = g;
}
function fill(force) {
  fillForce = !!force;
  $('mode-grbl').className =
    'segbtn' + ((S.controller_mode || 'grbl') === 'grbl' ? ' segon' : '');
  $('mode-cloud').className = 'segbtn' + (S.controller_mode === 'cloud' ? ' segon' : '');
  setF('ui_units', S.ui_units || 'metric');
  setF('homing_mode', S.homing_mode || 'none');
  setF('gfcloud_home_x', S.gfcloud_home_x);
  setF('gfcloud_home_y', S.gfcloud_home_y);
  setF('gfcloud_home_z', S.gfcloud_home_z);
  setF('gfcloud_home_timeout_s', S.gfcloud_home_timeout_s);
  setF('cloud_pause_backtrack_ticks', S.cloud_pause_backtrack_ticks);
  setF('cloud_resume_lead_ticks', S.cloud_resume_lead_ticks);
  setF('pulse_warn_threshold_bytes', S.pulse_warn_threshold_bytes);
  setF('pulse_reject_threshold_bytes', S.pulse_reject_threshold_bytes);
  for (var ci = 0; ci < CK.length; ci++) setF(CK[ci], S[CK[ci]]);
  setF('laser_button_timeout_s', S.laser_button_timeout_s);
  setF('laser_disarm_s', S.laser_disarm_s);
  setF('lid_policy', S.lid_policy || 'cancel');
  setF('rail_settle_s', S.rail_settle_s);
  setF('lid_lamp_idle', S.lid_lamp_idle);
  setF('wifi_country', S.wifi_country || '00');
  setF('gf_serial', S.gf_serial);
  if (fillForce) $('gf_password').value = '';
  $('gf_password').placeholder = S.gf_password_set
    ? '(override set: blank keeps it)'
    : '(factory fuses)';
  var uu = document.querySelectorAll('.ulen'),
    ui;
  for (ui = 0; ui < uu.length; ui++) uu[ui].textContent = uL();
  uu = document.querySelectorAll('.utemp');
  for (ui = 0; ui < uu.length; ui++) uu[ui].textContent = uT();
  for (var pk in PH)
    $(pk).placeholder = fnum(PH[pk][1] === 'ta' ? dTa(PH[pk][0]) : dTd(PH[pk][0]), 1);
  $('host').textContent = (S.machine_id || '') + (S.version ? ' \u00b7 ' + S.version : '');
  fillForce = false;
  renderGateNotes();
  renderGatesBanner();
  renderStat();
  updateSaveBar();
}
(function () {
  for (var k in GN) if ($(k)) $(k).addEventListener('input', renderGateNotes);
})();
/* An immediate settings write outside the save bar (an action button):
 * the reply refills the page, keeping any edit in progress. */
function postNow(pairs, msgId) {
  $(msgId).textContent = 'saving\u2026';
  postSettings(pairs)
    .then(function (s) {
      S = s;
      fill();
      $(msgId).textContent = 'saved';
      setTimeout(function () {
        $(msgId).textContent = '';
      }, 2500);
    })
    .catch(function (e) {
      $(msgId).textContent = String(e || 'save failed');
    });
}
function clearIdentity() {
  $('gf_serial').value = orig.gf_serial || '';
  $('gf_password').value = '';
  postNow({ gf_serial: '', gf_password: '' }, 'msg-i');
}
var RG =
  "AF Afghanistan;AX Aland Islands;AL Albania;DZ Algeria;AS American Samoa;AD Andorra;AO Angola;AI Anguilla;AG Antigua & Barbuda;AR Argentina;AM Armenia;AW Aruba;AU Australia;AT Austria;AZ Azerbaijan;BS Bahamas;BH Bahrain;BD Bangladesh;BB Barbados;BY Belarus;BE Belgium;BZ Belize;BJ Benin;BM Bermuda;BT Bhutan;BO Bolivia;BQ Bonaire, Sint Eustatius and Saba;BA Bosnia and Herzegovina;BW Botswana;BR Brazil;IO British Indian Ocean Territory;VG British Virgin Islands;BN Brunei;BG Bulgaria;BF Burkina Faso;BI Burundi;CV Cabo Verde;KH Cambodia;CM Cameroon;CA Canada;KY Cayman Islands;CF Central African Republic;TD Chad;CL Chile;CN China;CX Christmas Island;CC Cocos (Keeling) Islands;CO Colombia;KM Comoros;CG Congo;CD Congo (DRC);CK Cook Islands;CR Costa Rica;CI Cote d'Ivoire;HR Croatia;CU Cuba;CW Curacao;CY Cyprus;CZ Czechia;DK Denmark;DJ Djibouti;DM Dominica;DO Dominican Republic;EC Ecuador;EG Egypt;SV El Salvador;GQ Equatorial Guinea;ER Eritrea;EE Estonia;SZ Eswatini;ET Ethiopia;FK Falkland Islands;FO Faroe Islands;FJ Fiji;FI Finland;FR France;GF French Guiana;PF French Polynesia;GA Gabon;GM Gambia;GE Georgia;DE Germany;GH Ghana;GI Gibraltar;GR Greece;GL Greenland;GD Grenada;GP Guadeloupe;GU Guam;GT Guatemala;GG Guernsey;GN Guinea;GW Guinea-Bissau;GY Guyana;HT Haiti;HN Honduras;HK Hong Kong SAR;HU Hungary;IS Iceland;IN India;ID Indonesia;IR Iran;IQ Iraq;IE Ireland;IM Isle of Man;IL Israel;IT Italy;JM Jamaica;JP Japan;JE Jersey;JO Jordan;KZ Kazakhstan;KE Kenya;KI Kiribati;KR Korea;XK Kosovo;KW Kuwait;KG Kyrgyzstan;LA Laos;LV Latvia;LB Lebanon;LS Lesotho;LR Liberia;LY Libya;LI Liechtenstein;LT Lithuania;LU Luxembourg;MO Macao SAR;MG Madagascar;MW Malawi;MY Malaysia;MV Maldives;ML Mali;MT Malta;MH Marshall Islands;MQ Martinique;MR Mauritania;MU Mauritius;YT Mayotte;MX Mexico;FM Micronesia;MD Moldova;MC Monaco;MN Mongolia;ME Montenegro;MS Montserrat;MA Morocco;MZ Mozambique;MM Myanmar;NA Namibia;NR Nauru;NP Nepal;NL Netherlands;NC New Caledonia;NZ New Zealand;NI Nicaragua;NE Niger;NG Nigeria;NU Niue;NF Norfolk Island;KP North Korea;MK North Macedonia;MP Northern Mariana Islands;NO Norway;OM Oman;PK Pakistan;PW Palau;PS Palestinian Authority;PA Panama;PG Papua New Guinea;PY Paraguay;PE Peru;PH Philippines;PN Pitcairn Islands;PL Poland;PT Portugal;PR Puerto Rico;QA Qatar;RE Reunion;RO Romania;RU Russia;RW Rwanda;WS Samoa;SM San Marino;ST Sao Tome & Principe;SA Saudi Arabia;SN Senegal;RS Serbia;SC Seychelles;SL Sierra Leone;SG Singapore;SX Sint Maarten;SK Slovakia;SI Slovenia;SB Solomon Islands;SO Somalia;ZA South Africa;SS South Sudan;ES Spain;LK Sri Lanka;SH St Helena, Ascension, Tristan da Cunha;BL St. Barthelemy;KN St. Kitts & Nevis;LC St. Lucia;MF St. Martin;PM St. Pierre & Miquelon;VC St. Vincent & Grenadines;SD Sudan;SR Suriname;SJ Svalbard & Jan Mayen;SE Sweden;CH Switzerland;SY Syria;TW Taiwan;TJ Tajikistan;TZ Tanzania;TH Thailand;TL Timor-Leste;TG Togo;TK Tokelau;TO Tonga;TT Trinidad & Tobago;TN Tunisia;TR Turkiye;TM Turkmenistan;TC Turks & Caicos Islands;TV Tuvalu;UM U.S. Outlying Islands;VI U.S. Virgin Islands;UG Uganda;UA Ukraine;AE United Arab Emirates;GB United Kingdom;US United States;UY Uruguay;UZ Uzbekistan;VU Vanuatu;VA Vatican City;VE Venezuela;VN Vietnam;WF Wallis & Futuna;YE Yemen;ZM Zambia;ZW Zimbabwe";
(function () {
  var s = $('wifi_country'),
    a = RG.split(';'),
    i;
  s.add(new Option('Automatic \u2014 AP country, else World (00)', '00'));
  for (i = 0; i < a.length; i++)
    s.add(new Option(a[i].slice(3) + ' (' + a[i].slice(0, 2) + ')', a[i].slice(0, 2)));
})();
function showFuse() {
  fx('/fuse-identity')
    .then(function (r) {
      if (!r.ok)
        return r.json().then(function (j) {
          throw (j && j.error) || 'cannot read fuses';
        });
      return r.json();
    })
    .then(function (f) {
      var g = '';
      g += kv('Serial', "<span class='mono'>" + esc(f.serial || '?') + '</span>');
      g += kv('Hostname', "<span class='mono'>" + esc(f.hostname || '?') + '</span>');
      g += kv('Password', "<span class='mono brk'>" + esc(f.password || '?') + '</span>');
      $('fusekv').innerHTML = g;
      $('fusewrap').className = 'on';
    })
    .catch(function (e) {
      $('msg-i').textContent = String(e || 'cannot read fuses');
    });
}
function hideFuse() {
  $('fusewrap').className = '';
  $('fusekv').innerHTML = '';
}
$('fusewrap').onclick = function (e) {
  if (e.target === this) hideFuse();
};
function startDiag(t) {
  $('msg-d').textContent = 'starting\u2026';
  fx('/diag/' + t, { method: 'POST' })
    .then(function (r) {
      if (!r.ok)
        return r.text().then(function (x) {
          throw x;
        });
      $('msg-d').textContent = '';
      loadDiag();
    })
    .catch(function (e) {
      $('msg-d').textContent = String(e || 'failed');
    });
}
function abortDiag() {
  fx('/diag/abort', { method: 'POST' });
}
function applyRec(v) {
  postNow({ cool_flow_rise: String(v) }, 'msg-d');
}
function fmtEl(s) {
  return Math.floor(s / 60) + ':' + ('0' + (s % 60)).slice(-2);
}
function renderDiag() {
  var p = '',
    r = D.result,
    g = '';
  if (D.running) {
    $('diagabort').style.display = '';
    p += kv('Phase', esc(D.phase || ''));
    p += kv('Elapsed', fmtEl(D.elapsed_s || 0));
    p += kv(
      'Coolant',
      "<span class='mono'>down " +
        dTa(D.down_c || 0).toFixed(1) +
        ' / up ' +
        dTa(D.up_c || 0).toFixed(1) +
        ' ' +
        uT() +
        '</span>'
    );
  } else $('diagabort').style.display = 'none';
  $('diagpanel').innerHTML = p;
  var lg = $('diaglog');
  if (D.log && D.log.length) {
    lg.style.display = 'block';
    lg.textContent = D.log.join('\n');
    lg.scrollTop = lg.scrollHeight;
  } else lg.style.display = 'none';
  if (r && !D.running) {
    if (typeof r.pass !== 'undefined') {
      g += txt('Result', r.pass ? 'PASS' : 'FAIL', r.pass ? 'bigpass' : 'bigfail');
      g += txt(
        'Flow rise',
        dTd(r.flow_rise).toFixed(1) + ' ' + uT() + ' (dT ' + dTd(r.flow_dt).toFixed(1) + ')'
      );
      g += txt(
        'No-flow rise',
        dTd(r.noflow_rise).toFixed(1) + ' ' + uT() + ' (dT ' + dTd(r.noflow_dt).toFixed(1) + ')'
      );
      g += txt('Threshold', dTd(r.threshold).toFixed(1) + ' ' + uT());
      g += txt(
        'Margins',
        'flow ' +
          dTd(r.margin_flow).toFixed(1) +
          ' / no-flow ' +
          dTd(r.margin_noflow).toFixed(1) +
          ' ' +
          uT(),
        r.thin_margin ? 'b-warn' : 'b-ok'
      );
      if (r.thin_margin)
        g += txt(
          'Note',
          'margin under ' + fnum(dTd(1.5), 1) + ' ' + uT() + ' \u2014 run Calibrate',
          'b-warn'
        );
      if (!r.pass && !r.error)
        g += txt('Note', 'check pump and coolant, or run Calibrate', 'b-warn');
    }
    if (typeof r.gap !== 'undefined') {
      var f1 = function (x) {
        return dTd(x).toFixed(1);
      };
      g += txt(
        'Flow band',
        '\u2264 ' + f1(r.flow_max) + ' ' + uT() + '  (' + r.flow_rises.map(f1).join(', ') + ')'
      );
      g += txt(
        'No-flow band',
        '\u2265 ' +
          f1(r.noflow_min) +
          ' ' +
          uT() +
          '  (' +
          r.noflow_rises.map(f1).join(', ') +
          ')'
      );
      g += txt('Gap', f1(r.gap) + ' ' + uT(), r.recommend ? 'b-ok' : 'b-warn');
      if (r.recommend)
        g += kv(
          'Recommended threshold',
          "<span class='mono'>" +
            f1(r.recommend) +
            ' ' +
            uT() +
            '</span> ' +
            "<button class='btn btn-sm btn-primary' onclick='applyRec(" +
            r.recommend +
            ")'>Apply</button>"
        );
    }
    if (r.error) g += txt('Result', r.error, 'bigfail');
  }
  $('diagresult').innerHTML = g;
}
function loadDiag() {
  fetch('/diag/status')
    .then(function (r) {
      return r.json();
    })
    .then(function (d) {
      var was = !!D.running;
      D = d;
      renderDiag();
      if (was !== !!D.running) {
        locked = locked || D.running;
        lockApply();
      }
    })
    .catch(function () {});
}
var SL = {},
  jobTimer = null,
  lastApply = null;
var SLNAME = { sd: 'SD card', a: 'eMMC slot 1', b: 'eMMC slot 2', legacy: 'Legacy partition' };
function slotRow(n, s) {
  var b = '';
  if (s.booted) b += " <span class='b-ok'>booted</span>";
  if (s.next) b += " <span class='b-run'>next boot</span>";
  var lbl =
    s.present === 'yes'
      ? s.type
        ? esc(s.type) + ' ' + esc(s.version)
        : '(empty)'
      : '(not present)';
  var act =
    s.present === 'yes' && !s.next
      ? " <button class='btn btn-sm btn-outline-secondary' data-boot='" +
        n +
        "'>Set next boot</button>"
      : '';
  return (
    "<div class='kv'><span>" + SLNAME[n] + '</span><span>' + lbl + b + act + '</span></div>'
  );
}
function tgt() {
  var n = $('applyslot').value;
  if (!n) return null;
  var s = (SL.slots || {})[n] || {};
  return {
    slot: n,
    name: SLNAME[n] || n,
    cur: s.type ? esc(s.type) + ' ' + esc(s.version || '') : 'empty'
  };
}
function archName(a) {
  return a.version
    ? 'Factory v' + esc(a.version)
    : a.date
      ? 'Factory build ' + esc(a.date)
      : esc(a.file);
}
function renderSlots() {
  var g = '',
    n,
    sl = SL.slots || {};
  for (n in sl) g += slotRow(n, sl[n]);
  $('slottbl').innerHTML = g;
  var prev = $('applyslot').value,
    opts = [],
    t = ['a', 'b'],
    i;
  for (i = 0; i < t.length; i++) {
    var s = sl[t[i]] || {};
    if (s.booted || s.present !== 'yes') continue;
    opts.push({
      v: t[i],
      f: s.type === 'factory',
      label: SLNAME[t[i]] + ' \u2014 ' + esc(s.type || 'empty') + ' ' + esc(s.version || '')
    });
  }
  opts.sort(function (x, y) {
    return (x.f ? 1 : 0) - (y.f ? 1 : 0);
  });
  var sel = '';
  for (i = 0; i < opts.length; i++)
    sel += "<option value='" + opts[i].v + "'>" + opts[i].label + '</option>';
  $('applyslot').innerHTML = sel || "<option value=''>(none writable)</option>";
  if (prev) $('applyslot').value = prev;
  renderLists();
}
function renderLists() {
  var tg = tgt(),
    to = tg ? tg.name : 'the target slot',
    n,
    i;
  var st = SL.staged || {},
    g2 = '';
  var lb = { download: 'Downloaded release', upload: 'Uploaded archive' };
  for (n in lb) {
    var e = st[n] || {};
    if (!e.present) continue;
    g2 +=
      "<div class='kv'><span>" +
      lb[n] +
      "</span><span class='mono'>" +
      esc(e.version || '?') +
      ' \u00b7 ' +
      Math.round(e.bytes / 1048576) +
      ' MB' +
      " <button class='btn btn-sm btn-primary' data-apply='" +
      n +
      "' data-ver='" +
      esc(e.version || 'this image') +
      "'>Install to " +
      esc(to) +
      '</button></span></div>';
  }
  $('stagedlist').innerHTML = g2 || "<p class='hint'>Nothing staged yet.</p>";
  var ar = SL.archives || [],
    g3 = '';
  for (i = 0; i < ar.length; i++) {
    var a = ar[i];
    g3 +=
      "<div class='kv'><span>" +
      archName(a) +
      "</span><span class='mono'>" +
      Math.round(a.bytes / 1048576) +
      ' MB' +
      " <button class='btn btn-sm btn-outline-secondary' data-restore='" +
      esc(a.file) +
      "' data-ver='" +
      esc(a.version ? 'v' + a.version : a.date || a.file) +
      "'>Restore to " +
      esc(to) +
      '</button></span></div>';
  }
  $('archlist').innerHTML =
    g3 || "<p class='hint'>No factory archives on /data (the installer creates them).</p>";
}
function loadSlots() {
  fetch('/slots')
    .then(function (r) {
      return r.json();
    })
    .then(function (s) {
      SL = s;
      renderSlots();
    })
    .catch(function () {});
}
function bootTo(t, f) {
  fx('/boot?target=' + t + (f ? '&force=1' : ''), { method: 'POST' })
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (j.ok) {
        $('msg-slot').textContent = 'next boot set \u2014 reboot to switch';
        loadSlots();
      } else if (
        !f &&
        j.detail &&
        j.detail.indexOf('-f') >= 0 &&
        confirm('The probe refused this target:\n' + j.detail + '\nForce anyway?')
      )
        bootTo(t, 1);
      else $('msg-slot').textContent = (j.error || '?') + (j.detail ? ': ' + j.detail : '');
    });
}
function doReboot() {
  if (!confirm('Reboot the machine now?')) return;
  fx('/system/reboot?confirm=1', { method: 'POST' })
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      $('msg-slot').textContent = j.rebooting ? 'rebooting\u2026' : j.error || '?';
    });
}
function checkUpd() {
  $('msg-u').textContent = 'checking\u2026';
  fx('/update/check', { method: 'POST' })
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (j.error) {
        $('msg-u').textContent = j.error;
        return;
      }
      $('msg-u').textContent = '';
      var g =
        "<div class='kv'><span>Installed</span><span class='mono'>" +
        esc(j.current || '?') +
        '</span></div>';
      if (j.available)
        g +=
          "<div class='kv'><span>Latest release</span><span class='mono'>" +
          esc(j.version) +
          (j['new']
            ? " <span class='b-run'>update available</span>"
            : " <span class='b-ok'>up to date</span>") +
          '</span></div>';
      else
        g +=
          "<div class='kv'><span>Latest release</span><span>" +
          esc(j.detail || 'none') +
          '</span></div>';
      $('updinfo').innerHTML = g;
      $('dlbtn').style.display = j.available && j['new'] ? '' : 'none';
    })
    .catch(function () {
      $('msg-u').textContent = 'check failed';
    });
}
function jobPoll() {
  fetch('/update/status')
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      var g;
      if (j.running) {
        g =
          esc(j.kind) +
          ': ' +
          esc(j.phase || '\u2026') +
          ' (' +
          j.elapsed +
          's' +
          (j.progress_bytes > 0 ? ', ' + Math.round(j.progress_bytes / 1048576) + ' MB' : '') +
          ')';
        if (!jobTimer) jobTimer = setInterval(jobPoll, 2000);
      } else {
        if (jobTimer) {
          clearInterval(jobTimer);
          jobTimer = null;
          loadSlots();
        }
        if (
          j.result &&
          j.result.ok === false &&
          String(j.result.error || '').indexOf('confirm_unsigned') >= 0 &&
          lastApply &&
          confirm('The archive is not signed with the release key.\nInstall it anyway?')
        ) {
          startJob(
            '/update/apply',
            'file=' + lastApply.f + '&slot=' + lastApply.s + '&confirm_unsigned=1',
            'msg-up'
          );
          lastApply = null;
          return;
        }
        g = j.result
          ? j.result.ok
            ? "<span class='b-ok'>" +
              esc(j.kind) +
              ' ok' +
              (j.result.version ? ' \u2014 ' + esc(j.result.version) : '') +
              '</span>'
            : "<span class='b-bad'>" + esc(j.result.error || 'failed') + '</span>'
          : 'no job running';
      }
      $('jobpanel').innerHTML = g;
    })
    .catch(function () {});
}
function startJob(url, q, msgid) {
  $(msgid).textContent = '';
  fx(url + (q ? '?' + q : ''), { method: 'POST' })
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (j.started) jobPoll();
      else $(msgid).textContent = j.error || '?';
    });
}
function dlUpd() {
  startJob('/update/download', '', 'msg-u');
}
function applyStaged(f, ver) {
  var t = tgt();
  if (!t) {
    $('msg-up').textContent = 'no writable target slot';
    return;
  }
  if (
    !confirm(
      'Install ' +
        ver +
        ' to ' +
        t.name +
        '?\nThis ERASES its current contents (' +
        t.cur +
        ').'
    )
  )
    return;
  lastApply = { f: f, s: t.slot };
  startJob('/update/apply', 'file=' + f + '&slot=' + t.slot, 'msg-up');
}
function restoreArch(f, ver) {
  var t = tgt();
  if (!t) {
    $('msg-r').textContent = 'no writable target slot';
    return;
  }
  if (
    !confirm(
      'Restore factory ' +
        ver +
        ' to ' +
        t.name +
        '?\nThis ERASES its current contents (' +
        t.cur +
        ').'
    )
  )
    return;
  startJob('/restore/factory', 'file=' + encodeURIComponent(f) + '&slot=' + t.slot, 'msg-r');
}
function uploadFw() {
  var fi = $('fwfile');
  if (!fi.files || !fi.files.length) {
    $('msg-up').textContent = 'choose a .fw file';
    return;
  }
  var fd = new FormData();
  fd.append('file', fi.files[0]);
  $('msg-up').textContent = 'uploading\u2026';
  fx('/update/upload', { method: 'POST', body: fd })
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (j.ok) {
        $('msg-up').textContent = 'uploaded ' + (j.version || '') + ' (' + j.signature + ')';
        loadSlots();
      } else $('msg-up').textContent = j.error || 'upload failed';
    })
    .catch(function () {
      $('msg-up').textContent = 'upload failed';
    });
}
var LG = {},
  logName = 'forgectrl',
  logOff = -1,
  logTimer = null;
function fmtBytes(b) {
  return b < 1024
    ? b + ' B'
    : b < 1048576
      ? Math.round(b / 1024) + ' KB'
      : (b / 1048576).toFixed(1) + ' MB';
}
function lvlSel(id, cur) {
  var L = LG.levels || [],
    o = '',
    i;
  for (i = 0; i < L.length; i++)
    o +=
      "<option value='" +
      L[i] +
      "'" +
      (L[i] === cur ? ' selected' : '') +
      '>' +
      L[i] +
      '</option>';
  return "<select class='form-select form-select-sm' id='" + id + "'>" + o + '</select>';
}
function renderLogs() {
  if (!LG.loggers) return;
  var g = '',
    i,
    l,
    pd;
  for (i = 0; i < LG.loggers.length; i++) {
    l = LG.loggers[i];
    pd = LG.effective_known && (l.disk !== l.effective_disk || l.remote !== l.effective_remote);
    g +=
      '<tr><td>' +
      esc(l.name) +
      (pd ? " <span class='b-warn' title='reboot to apply'>*</span>" : '') +
      '</td><td>' +
      lvlSel('ld-' + l.name, l.disk) +
      '</td><td>' +
      lvlSel('lr-' + l.name, l.remote) +
      "</td><td class='mono'>" +
      fmtBytes(l.bytes) +
      '</td></tr>';
  }
  $('logrows').innerHTML = g;
  $('syslog_server').value = LG.syslog_server || '';
  $('syslog_port').value = LG.syslog_port || '';
  $('syslog_proto').value = LG.syslog_proto || 'udp';
  $('logpending').style.display = LG.pending_reboot ? '' : 'none';
  if (!$('logsel').options.length) {
    var o = '';
    for (i = 0; i < LG.loggers.length; i++)
      o += "<option value='" + LG.loggers[i].name + "'>" + LG.loggers[i].name + '</option>';
    $('logsel').innerHTML = o;
    $('logsel').value = logName;
  }
  lockApply();
}
function loadLogs() {
  fx('/logs')
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      LG = j;
      renderLogs();
    })
    .catch(function () {});
}
function loadTail(reset) {
  var n = $('logsel').value || logName,
    ln = $('loglines').value,
    v = $('logview');
  if (n !== logName || reset) {
    logName = n;
    logOff = -1;
    v.textContent = '';
  }
  fx('/logs/tail?name=' + n + '&lines=' + ln + (logOff >= 0 ? '&from=' + logOff : ''))
    .then(function (r) {
      return r.json();
    })
    .then(function (j) {
      if (!j.exists) {
        v.textContent = '(no log yet)';
        logOff = -1;
        return;
      }
      if (logOff >= 0 && j.offset >= logOff && !j.truncated) {
        if (j.text) v.textContent += j.text;
      } else v.textContent = j.text;
      if (v.textContent.length > 300000) {
        var t = v.textContent,
          c = t.indexOf('\n', t.length - 200000);
        v.textContent = c > 0 ? t.slice(c + 1) : t;
      }
      logOff = j.offset;
      v.scrollTop = v.scrollHeight;
      $('msg-lv').textContent = '';
    })
    .catch(function () {
      $('msg-lv').textContent = 'no response';
    });
}
function toggleFollow() {
  if (logTimer) {
    clearInterval(logTimer);
    logTimer = null;
    $('logfollow').classList.remove('on');
  } else {
    logTimer = setInterval(function () {
      loadTail(false);
    }, 2000);
    $('logfollow').classList.add('on');
    loadTail(false);
  }
}
function exportLogs() {
  var san = $('logsan').checked;
  $('msg-lx').textContent = 'building the bundle\u2026';
  fx('/logs/export?sanitize=' + (san ? '1' : '0'), { method: 'POST' })
    .then(function (r) {
      if (!r.ok)
        return r.text().then(function (t) {
          throw t;
        });
      var cd = r.headers.get('Content-Disposition') || '',
        m = /filename="([^"]+)"/.exec(cd);
      return r.blob().then(function (b) {
        return { b: b, n: m ? m[1] : 'forgefirm-logs.tar.gz' };
      });
    })
    .then(function (o) {
      var u = URL.createObjectURL(o.b),
        a = document.createElement('a');
      a.href = u;
      a.download = o.n;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      setTimeout(function () {
        URL.revokeObjectURL(u);
      }, 10000);
      $('msg-lx').textContent = o.n + ' (' + fmtBytes(o.b.size) + ')';
    })
    .catch(function (e) {
      $('msg-lx').textContent = String(e || 'export failed');
    });
}
document.addEventListener('click', function (e) {
  var t = e.target,
    v;
  if (!t || !t.getAttribute) return;
  if ((v = t.getAttribute('data-boot'))) bootTo(v, 0);
  else if ((v = t.getAttribute('data-apply'))) applyStaged(v, t.getAttribute('data-ver'));
  else if ((v = t.getAttribute('data-restore'))) restoreArch(v, t.getAttribute('data-ver'));
});
function renderGfSvc() {
  var b = $('gfsvc-banner'),
    g = M.gfsvc;
  if (g && g.latest && g.tested && g.latest !== g.tested) {
    b.innerHTML =
      '\u26A0 Cloud mode: Glowforge firmware is now ' +
      esc(g.latest) +
      '; this ForgeFIRM release was tested against ' +
      esc(g.tested) +
      '. Cloud mode may not work correctly. ' +
      "<a href='#system'>Check for a ForgeFIRM update</a>";
    b.style.display = '';
  } else {
    b.style.display = 'none';
  }
}
function loadMach() {
  fetch('/status')
    .then(function (r) {
      return r.json();
    })
    .then(function (m) {
      M = m;
      locked = !!(M.state && M.state !== 'idle') || !!M.diag;
      lockApply();
      renderMotion();
      renderCooling();
      renderSwitches();
      renderGfSvc();
    })
    .catch(function () {});
}
function loadCam() {
  fetch('/cam/status')
    .then(function (r) {
      return r.json();
    })
    .then(function (s) {
      CS = s;
      renderStat();
    })
    .catch(function () {});
}
fetch('/settings')
  .then(function (r) {
    return r.json();
  })
  .then(function (s) {
    S = s;
    fill(true);
  })
  .catch(function () {});
initHelp();
loadMach();
setInterval(loadMach, 2500);
loadCam();
setInterval(loadCam, 5000);
loadDiag();
setInterval(loadDiag, 2500);
loadSlots();
jobPoll();
renderGrbl();
tab();
refreshSnap();

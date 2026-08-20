/*
 * panel.js - forgectrl: the control panel
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Single-page machine control panel, hash-routed tabs, ES5 only:
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
 * in one of these tabs as it appears.
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
  'cool_cooldown_s',
  'cool_cooldown_max_s'
];
function $(i) {
  return document.getElementById(i);
}
function esc(t) {
  return String(t)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}
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
  cool_temp_resume: 'ta'
};
var PH = {
  cool_flow_rise: [14.4, 'td'],
  cool_temp_max: [33, 'ta'],
  cool_temp_resume: [31, 'ta']
};
var orig = {};
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
function setF(k, raw) {
  var v = dspVal(k, raw || '');
  $(k).value = v;
  orig[k] = v;
}
function dirty(k) {
  return $(k).value.trim() !== orig[k];
}
function pick(p, keys) {
  var i,
    k,
    any = 0;
  for (i = 0; i < keys.length; i++) {
    k = keys[i];
    if (dirty(k)) {
      p[k] = backVal(k, $(k).value.trim());
      any = 1;
    }
  }
  return any;
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
      ? 'A diagnostic is running \u2014 controls are locked until it completes.'
      : 'Settings are locked while the machine is busy (state: ' +
        (M.state || 'unknown') +
        ').';
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
  sysChecked = false;
function tab() {
  var h = location.hash.replace('#', '');
  if (TABS.indexOf(h) < 0) h = 'status';
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
window.onhashchange = tab;
function snapUrl() {
  return '/cam/snapshot?cam=lid&res=half&t=' + Date.now();
}
function refreshSnap() {
  if (!liveOn) $('cam').src = snapUrl();
}
function toggleLive() {
  liveOn = !liveOn;
  retries = 0;
  $('cammsg').textContent = '';
  $('live').textContent = liveOn ? 'Stop' : 'Live';
  $('live').className = liveOn ? 'on' : '';
  $('snapbtn').disabled = liveOn;
  $('cam').src = liveOn ? '/cam/stream?cam=lid&t=' + Date.now() : snapUrl();
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
function fill() {
  $('mode-grbl').className =
    'segbtn' + ((S.controller_mode || 'grbl') === 'grbl' ? ' segon' : '');
  $('mode-cloud').className = 'segbtn' + (S.controller_mode === 'cloud' ? ' segon' : '');
  $('ui_units').value = S.ui_units || 'metric';
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
  $('gf_password').value = '';
  $('gf_password').placeholder = S.gf_password_set
    ? '(override set \u2014 blank keeps it)'
    : '(factory fuses)';
  var uu = document.querySelectorAll('.ulen'),
    ui;
  for (ui = 0; ui < uu.length; ui++) uu[ui].textContent = uL();
  uu = document.querySelectorAll('.utemp');
  for (ui = 0; ui < uu.length; ui++) uu[ui].textContent = uT();
  for (var pk in PH)
    $(pk).placeholder = fnum(PH[pk][1] === 'ta' ? dTa(PH[pk][0]) : dTd(PH[pk][0]), 1);
  $('host').textContent = (S.machine_id || '') + (S.version ? ' \u00b7 ' + S.version : '');
  renderStat();
}
function post(pairs, msgId) {
  var q = '',
    k;
  for (k in pairs)
    q += (q ? '&' : '') + encodeURIComponent(k) + '=' + encodeURIComponent(pairs[k]);
  $(msgId).textContent = 'saving\u2026';
  fx('/settings?' + q, { method: 'POST' })
    .then(function (r) {
      if (!r.ok)
        return r.text().then(function (t) {
          throw t;
        });
      return r.json();
    })
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
function saveUnits() {
  post({ ui_units: $('ui_units').value }, 'msg-u');
}
function saveMachine() {
  var p = {};
  if (!pick(p, ['homing_mode', 'gfcloud_home_x', 'gfcloud_home_y', 'gfcloud_home_z'])) {
    $('msg-m').textContent = 'no changes';
    return;
  }
  post(p, 'msg-m');
}
function saveCooling() {
  var p = {};
  if (!pick(p, CK)) {
    $('msg-c').textContent = 'no changes';
    return;
  }
  post(p, 'msg-c');
}
function saveGrbl() {
  var p = {};
  if (!pick(p, ['laser_button_timeout_s', 'laser_disarm_s'])) {
    $('msg-g').textContent = 'no changes';
    return;
  }
  post(p, 'msg-g');
}
function saveLid() {
  var p = {};
  if (!pick(p, ['lid_policy'])) {
    $('msg-l').textContent = 'no changes';
    return;
  }
  post(p, 'msg-l');
}
function saveRail() {
  var p = {};
  if (!pick(p, ['rail_settle_s'])) {
    $('msg-r').textContent = 'no changes';
    return;
  }
  post(p, 'msg-r');
}
function saveLamp() {
  var p = {};
  if (!pick(p, ['lid_lamp_idle'])) {
    $('msg-lamp').textContent = 'no changes';
    return;
  }
  post(p, 'msg-lamp');
}
function saveIdentity() {
  var p = {};
  pick(p, ['gf_serial']);
  if ($('gf_password').value) p.gf_password = $('gf_password').value.trim();
  if (!Object.keys(p).length) {
    $('msg-i').textContent = 'no changes';
    return;
  }
  post(p, 'msg-i');
}
function clearIdentity() {
  post({ gf_serial: '', gf_password: '' }, 'msg-i');
}
function saveSession() {
  var p = {};
  if (!pick(p, ['gfcloud_home_timeout_s'])) {
    $('msg-s').textContent = 'no changes';
    return;
  }
  post(p, 'msg-s');
}
function savePause() {
  var p = {};
  if (!pick(p, ['cloud_pause_backtrack_ticks', 'cloud_resume_lead_ticks'])) {
    $('msg-p').textContent = 'no changes';
    return;
  }
  post(p, 'msg-p');
}
function saveJobSize() {
  var p = {};
  if (!pick(p, ['pulse_warn_threshold_bytes', 'pulse_reject_threshold_bytes'])) {
    $('msg-js').textContent = 'no changes';
    return;
  }
  post(p, 'msg-js');
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
function saveWifi() {
  var p = {};
  if (!pick(p, ['wifi_country'])) {
    $('msg-w').textContent = 'no changes';
    return;
  }
  post(p, 'msg-w');
}
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
  post({ cool_flow_rise: String(v) }, 'msg-d');
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
            "<button class='pri' onclick='applyRec(" +
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
      ? " <button data-boot='" + n + "'>Set next boot</button>"
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
      " <button class='pri' data-apply='" +
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
      " <button data-restore='" +
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
  return "<select id='" + id + "'>" + o + '</select>';
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
function postLogs(p, msgId) {
  var q = '',
    k;
  for (k in p) q += (q ? '&' : '') + encodeURIComponent(k) + '=' + encodeURIComponent(p[k]);
  $(msgId).textContent = 'saving\u2026';
  fx('/settings?' + q, { method: 'POST' })
    .then(function (r) {
      if (!r.ok)
        return r.text().then(function (t) {
          throw t;
        });
      return r.json();
    })
    .then(function (s) {
      S = s;
      $(msgId).textContent = 'saved \u2014 applies at the next reboot';
      loadLogs();
    })
    .catch(function (e) {
      $(msgId).textContent = String(e || 'save failed');
    });
}
function saveLogLevels() {
  var p = {},
    i,
    l,
    d,
    r;
  if (!LG.loggers) return;
  for (i = 0; i < LG.loggers.length; i++) {
    l = LG.loggers[i];
    d = $('ld-' + l.name).value;
    r = $('lr-' + l.name).value;
    if (d !== l.disk) p['log_' + l.name + '_disk'] = d;
    if (r !== l.remote) p['log_' + l.name + '_remote'] = r;
  }
  if (!Object.keys(p).length) {
    $('msg-ll').textContent = 'no changes';
    return;
  }
  postLogs(p, 'msg-ll');
}
function saveSyslog() {
  var p = {},
    s = $('syslog_server').value.trim(),
    pt = $('syslog_port').value.trim(),
    pr = $('syslog_proto').value;
  if (s !== (LG.syslog_server || '')) p.syslog_server = s;
  if (pt !== (LG.syslog_port || '')) p.syslog_port = pt;
  if (pr !== (LG.syslog_proto || 'udp')) p.syslog_proto = pr;
  if (!Object.keys(p).length) {
    $('msg-ls').textContent = 'no changes';
    return;
  }
  postLogs(p, 'msg-ls');
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
    $('logfollow').className = '';
  } else {
    logTimer = setInterval(function () {
      loadTail(false);
    }, 2000);
    $('logfollow').className = 'on';
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
    fill();
  })
  .catch(function () {});
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

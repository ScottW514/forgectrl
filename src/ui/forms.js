/*
 * forms.js - forgectrl: one save for every setting, the theme, the toasts
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The settings fields on every tab share one dirty set and one Save. A
 * field is dirty when its displayed text differs from what the server
 * last reported (orig[], kept by setF in panel.js); the log-level and
 * syslog fields compare against the /logs reply (LG) instead, because
 * that is where they are read from. While anything is dirty the save bar
 * shows at the foot of the page; Save posts every dirty key in one
 * request to POST /settings, and leaving the tab, or the page, with
 * unsaved changes asks first.
 *
 * The password field is dirty whenever it holds text (a blank keeps the
 * stored override, so blank is never a change). Everything that is not a
 * setting (the mode switch, diagnostics, firmware operations, the fuse
 * viewer, "Use factory identity") acts immediately and never enters the
 * dirty set.
 */
var FIELDS_BASE = [
  'ui_units',
  'homing_mode',
  'gfcloud_home_x',
  'gfcloud_home_y',
  'gfcloud_home_z',
  'gfcloud_home_timeout_s',
  'cloud_pause_backtrack_ticks',
  'cloud_resume_lead_ticks',
  'pulse_warn_threshold_bytes',
  'pulse_reject_threshold_bytes',
  'laser_button_timeout_s',
  'laser_disarm_s',
  'laser_floor_density',
  'laser_dose_curve',
  'laser_corner_gamma',
  'laser_pulse_ticks',
  'laser_pulse_min_ticks',
  'lid_policy',
  'rail_settle_s',
  'lid_lamp_idle',
  'wifi_country',
  'gf_serial'
];
function fieldKeys() {
  return FIELDS_BASE.concat(typeof CK !== 'undefined' ? CK : []);
}
/* When a saved key takes effect, for the toast after a save. */
function appliesWhen(k) {
  if (k.indexOf('log_') === 0 || k.indexOf('syslog_') === 0) return 'reboot';
  if (k.indexOf('cool_') === 0 || k.indexOf('laser_') === 0) return 'job';
  return 'now';
}
function logPairs() {
  var p = {},
    i,
    l,
    d,
    r;
  if (typeof LG === 'undefined' || !LG.loggers) return p;
  for (i = 0; i < LG.loggers.length; i++) {
    l = LG.loggers[i];
    d = $('ld-' + l.name);
    r = $('lr-' + l.name);
    if (d && d.value !== l.disk) p['log_' + l.name + '_disk'] = d.value;
    if (r && r.value !== l.remote) p['log_' + l.name + '_remote'] = r.value;
  }
  var s = $('syslog_server').value.trim(),
    pt = $('syslog_port').value.trim(),
    pr = $('syslog_proto').value;
  if (s !== (LG.syslog_server || '')) p.syslog_server = s;
  if (pt !== (LG.syslog_port || '')) p.syslog_port = pt;
  if (pr !== (LG.syslog_proto || 'udp')) p.syslog_proto = pr;
  return p;
}
/* Every dirty key with the value to post, converted back to metric. */
function collect() {
  var p = {},
    keys = fieldKeys(),
    i,
    k;
  for (i = 0; i < keys.length; i++) {
    k = keys[i];
    if ($(k) && dirty(k)) p[k] = backVal(k, $(k).value.trim());
  }
  if ($('gf_password').value) p.gf_password = $('gf_password').value.trim();
  var lp = logPairs();
  for (k in lp) p[k] = lp[k];
  return p;
}
function dirtyCount() {
  return Object.keys(collect()).length;
}
function markDirty() {
  var keys = fieldKeys(),
    i,
    k,
    e;
  for (i = 0; i < keys.length; i++) {
    k = keys[i];
    e = $(k);
    if (e) e.classList.toggle('dirty', dirty(k));
  }
  $('gf_password').classList.toggle('dirty', !!$('gf_password').value);
  if (typeof LG !== 'undefined' && LG.loggers)
    for (i = 0; i < LG.loggers.length; i++) {
      k = LG.loggers[i];
      e = $('ld-' + k.name);
      if (e) e.classList.toggle('dirty', e.value !== k.disk);
      e = $('lr-' + k.name);
      if (e) e.classList.toggle('dirty', e.value !== k.remote);
    }
}
function updateSaveBar() {
  var n = dirtyCount(),
    on = n > 0,
    busy = typeof locked !== 'undefined' && locked;
  markDirty();
  $('savebar').classList.toggle('on', on);
  document.body.classList.toggle('has-savebar', on);
  $('sb-count').textContent = n + ' unsaved change' + (n === 1 ? '' : 's');
  $('sb-save').disabled = busy;
  $('sb-discard').disabled = false;
  $('sb-save').title = busy ? 'settings are locked while the machine is busy' : '';
  if (!on) $('sb-msg').textContent = '';
}
/* POST /settings; resolves with the settings the daemon replies with,
 * rejects with the daemon's message. */
function postSettings(pairs) {
  var q = '',
    k;
  for (k in pairs)
    q += (q ? '&' : '') + encodeURIComponent(k) + '=' + encodeURIComponent(pairs[k]);
  return fx('/settings?' + q, { method: 'POST' }).then(function (r) {
    if (!r.ok)
      return r.text().then(function (t) {
        throw t;
      });
    return r.json();
  });
}
function saveAll(cb) {
  var p = collect(),
    keys = Object.keys(p),
    when = { reboot: 0, job: 0, now: 0 },
    i;
  if (!keys.length) {
    if (cb) cb();
    return;
  }
  for (i = 0; i < keys.length; i++) when[appliesWhen(keys[i])]++;
  $('sb-msg').textContent = 'saving\u2026';
  $('sb-save').disabled = true;
  $('sb-discard').disabled = true;
  postSettings(p)
    .then(function (s) {
      S = s;
      var touchedLogs = when.reboot > 0;
      if (touchedLogs && typeof LG !== 'undefined' && LG.loggers) {
        for (i = 0; i < keys.length; i++) {
          if (keys[i].indexOf('syslog_') === 0) LG[keys[i]] = p[keys[i]];
          else if (keys[i].indexOf('log_') === 0) {
            var m = /^log_(.+)_(disk|remote)$/.exec(keys[i]),
              j;
            for (j = 0; m && j < LG.loggers.length; j++)
              if (LG.loggers[j].name === m[1]) LG.loggers[j][m[2]] = p[keys[i]];
          }
        }
      }
      fill(true);
      if (touchedLogs) loadLogs();
      updateSaveBar();
      var notes = [];
      if (when.reboot) notes.push('logging changes apply at the next reboot');
      if (when.job) notes.push('cooling changes apply from the next job');
      toast(
        'Saved ' + keys.length + ' setting' + (keys.length === 1 ? '' : 's'),
        notes.join('; '),
        true
      );
      if (cb) cb();
    })
    .catch(function (e) {
      $('sb-msg').textContent = String(e || 'save failed');
      updateSaveBar();
    });
}
function discardAll() {
  var keys = fieldKeys(),
    i,
    k;
  for (i = 0; i < keys.length; i++) {
    k = keys[i];
    if ($(k) && typeof orig[k] !== 'undefined') $(k).value = orig[k];
  }
  $('gf_password').value = '';
  if (typeof LG !== 'undefined' && LG.loggers) renderLogs();
  renderGateNotes();
  updateSaveBar();
}

/* ---- The tab guard --------------------------------------------------
 * Leaving a tab with unsaved changes asks: save them, drop them, or
 * stay. The hash router in panel.js calls navGuard() with what to do
 * once the answer allows the move. */
var guardModal = null,
  guardGo = null,
  guardCloseWanted = false;
/* Bootstrap ignores hide() while the show transition is still running,
 * and a save round-trip or an instant Discard can land inside it; the
 * flag lets shown.bs.modal finish the close. */
function closeGuard() {
  guardCloseWanted = true;
  guardModal.hide();
}
function navGuard(proceed) {
  var n = dirtyCount();
  if (!n) {
    proceed();
    return;
  }
  guardGo = proceed;
  $('guard-body').textContent =
    'This tab has ' +
    n +
    ' unsaved change' +
    (n === 1 ? '' : 's') +
    '. Save before leaving, or discard?';
  $('guard-save').disabled = typeof locked !== 'undefined' && locked;
  if (!guardModal) guardModal = new bootstrap.Modal($('guard'));
  guardModal.show();
}
(function () {
  $('guard-save').addEventListener('click', function () {
    var go = guardGo;
    saveAll(function () {
      if (!dirtyCount()) {
        closeGuard();
        if (go) go();
      }
    });
  });
  $('guard-discard').addEventListener('click', function () {
    var go = guardGo;
    discardAll();
    closeGuard();
    if (go) go();
  });
  $('guard').addEventListener('shown.bs.modal', function () {
    if (guardCloseWanted) guardModal.hide();
  });
  $('guard').addEventListener('hidden.bs.modal', function () {
    guardGo = null;
    guardCloseWanted = false;
  });
  window.addEventListener('beforeunload', function (e) {
    if (dirtyCount()) {
      e.preventDefault();
      e.returnValue = '';
    }
  });
  /* Any edit anywhere on the page re-evaluates the dirty set. */
  document.addEventListener('input', function (e) {
    if (e.target && e.target.closest && e.target.closest('main')) updateSaveBar();
  });
  document.addEventListener('change', function (e) {
    if (e.target && e.target.closest && e.target.closest('main')) updateSaveBar();
  });
})();

/* ---- Toasts --------------------------------------------------------- */
function toast(title, body, ok) {
  var el = document.createElement('div');
  el.className = 'toast ' + (ok ? 't-ok' : 't-bad');
  el.setAttribute('role', 'status');
  el.innerHTML =
    "<div class='toast-body'><b>" + esc(title) + '</b>' + (body ? '<br>' + esc(body) : '') + '</div>';
  $('toasts').appendChild(el);
  el.addEventListener('hidden.bs.toast', function () {
    el.remove();
  });
  new bootstrap.Toast(el, { delay: ok ? 3500 : 8000 }).show();
}

/* ---- Theme ----------------------------------------------------------
 * Light, dark, or auto (the system preference); the choice lives in
 * localStorage. The head script in index.html applies it before first
 * paint; this is the same resolution for the toggle and for a system
 * preference that changes while the page is open. */
var THEMES = ['auto', 'light', 'dark'];
function themeChoice() {
  try {
    var s = localStorage.getItem('ff_theme');
    return s === 'light' || s === 'dark' ? s : 'auto';
  } catch (e) {
    return 'auto';
  }
}
function applyTheme() {
  var c = themeChoice(),
    t = c;
  if (c === 'auto')
    t = window.matchMedia && matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  document.documentElement.setAttribute('data-bs-theme', t);
  $('themebtn').textContent =
    c === 'auto' ? '\u25d0 Auto' : c === 'dark' ? '\u263e Dark' : '\u2600 Light';
  $('themebtn').title = 'Theme: ' + c + ' (click to change)';
}
function cycleTheme() {
  var next = THEMES[(THEMES.indexOf(themeChoice()) + 1) % THEMES.length];
  try {
    if (next === 'auto') localStorage.removeItem('ff_theme');
    else localStorage.setItem('ff_theme', next);
  } catch (e) {}
  applyTheme();
}
(function () {
  applyTheme();
  if (window.matchMedia) {
    var mq = matchMedia('(prefers-color-scheme: dark)');
    if (mq.addEventListener) mq.addEventListener('change', applyTheme);
    else if (mq.addListener) mq.addListener(applyTheme);
  }
})();

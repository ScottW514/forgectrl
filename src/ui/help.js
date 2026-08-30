/*
 * help.js - forgectrl: the panel's help text
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Every explanation the panel offers lives here, keyed by the data-help
 * attribute of the "?" button that opens it (index.html). Each entry is
 * a title, its paragraphs, and the page of the documentation site it
 * deep-links to (DOC_BASE + d). The buttons open Bootstrap popovers, one
 * at a time; a click anywhere else, Escape, or a tab change closes the
 * open one. What must be read without asking (the safety banner, the
 * gate warnings, the diagnostics takeover notice) stays in the page and
 * never moves here.
 */
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
var DOC_BASE = 'https://docs.openglow.org/forgefirm/';
var HELP = {
  mode: {
    t: 'Controller mode',
    d: 'panel/status#controller-mode',
    p: [
      'GRBL serves standard Grbl senders on port 23. Factory cloud runs the machine against the Glowforge web service like stock firmware.',
      'The two are mutually exclusive: switching stops one controller and starts the other (the machine must be idle) and persists across reboots.'
    ]
  },
  motion: {
    t: 'Motion',
    d: 'panel/status#motion',
    p: [
      'The controller state and the head position from the homing-anchored step counters. Position counters are not proof of physical motion; the head accelerometer is what the motion-liveness gate watches.',
      'The laser latch is the commanded state of the kernel lock; the emission line is what the beam sensor actually sees.'
    ]
  },
  switches: {
    t: 'Switches',
    d: 'panel/status#switches',
    p: [
      'The safety-chain readbacks. HV enable is the readback of the chain\'s HV_ENABLE output: on only while a run feeds the charge-pump watchdog with the lid closed.',
      'An open lid or an open remote interlock cuts the beam in hardware, whatever the software is doing.'
    ]
  },
  camera: {
    t: 'Lid camera',
    d: 'panel/status#lid-camera',
    p: [
      'A scaled snapshot, refreshed on demand; Live switches to the stream (H.264 when the browser supports it, MJPEG otherwise) and Stop returns to the snapshot.',
      'The cameras are off while the lid is open; that is the privacy gate, not a fault. One viewer at a time holds the stream.'
    ]
  },
  units: {
    t: 'Display units',
    d: 'panel/machine#units',
    p: ['Display only: values are stored and exchanged in metric, and conversion happens at the edge of the panel.']
  },
  homing: {
    t: 'Homing',
    d: 'panel/machine#homing',
    p: [
      'How the machine finds its origin. Glowforge web-service homing uses the cameras and the service, like stock firmware; limit-switch homing needs the switch brackets fitted.'
    ]
  },
  home_pos: {
    t: 'Home position',
    d: 'panel/machine#home-position',
    p: [
      'Machine coordinates the head is at after a completed homing cycle. For Glowforge web-service homing that is the factory home corner (back left) with the lens at the hall reference; leave blank until a measurement says otherwise.',
      'To calibrate: home, jog to a known reference, and enter the measured offsets.'
    ]
  },
  cooling: {
    t: 'Cooling protection',
    d: 'panel/machine#cooling-protection',
    p: [
      'Blank fields use the built-in values (the placeholders), measured on a factory machine; changes apply from the next job. The bands are per-machine: after a pump or coolant change, run Diagnostics, Cooling system, to verify or re-measure them.',
      'Every gate accepts a wide range on purpose: each has a recommended band, a value outside it is flagged under the fields, and the far end of the range (a ceiling at its maximum, a check window or a floor of 0) turns that gate off. A machine with a gate off says so on the Status tab and in its log at every job start.'
    ]
  },
  coolant: {
    t: 'Coolant loop',
    d: 'panel/machine#coolant-loop',
    p: [
      'The coolant ceiling pauses a job until the loop is back under the resume gate. The critical line above it is a fault that holds the job with no resume, like a stopped fan.',
      'Smoke clear is how long the fans keep running after a job; the cooldown limit caps that run.'
    ]
  },
  flow: {
    t: 'Flow verification',
    d: 'panel/machine#flow-verification',
    p: [
      'Each running job periodically verifies coolant flow by running the loop heater at the check duty for the check window and reading how far the downstream sensor climbs: past the fault rise means stagnant coolant.',
      'A suspicious reading is re-checked every re-check interval until the suspicion budget runs out, at which point the job is held.'
    ]
  },
  gates: {
    t: 'Airflow gates',
    d: 'panel/machine#airflow-gates',
    p: [
      'While a job runs, each fan is held to its floor (the exhaust, the intakes and the air assist by tachometer, the purge fan by its current) once the spin-up grace has passed; three seconds under a floor is a fault that holds the job with no resume.',
      'The floors were measured at the cut fan profile, so a fan is judged while the laser is armed or whenever it is commanded at that profile; a cloud hunt, which runs with its extraction fans off, is measured but not judged.'
    ]
  },
  identity: {
    t: 'Machine identity',
    d: 'panel/cloud#machine-identity',
    p: [
      'Credentials the machine signs in to the Glowforge web service with. Blank fields use the identity burned into the factory fuses; override only to stand in for another machine (64 hex digit password; the service hostname derives from the serial).',
      'Keep credentials secret; they cannot be changed once leaked. A blank password field keeps the current override.'
    ]
  },
  session: {
    t: 'Homing session',
    d: 'panel/cloud#homing-session',
    p: [
      'Overall budget for one web-service homing run (sign-in, camera uploads, hunt and corner moves). The controller aborts and alarms past this; 30 to 3600 seconds.'
    ]
  },
  pause: {
    t: 'Print pause',
    d: 'panel/cloud#print-pause',
    p: [
      'Pressing the button during a cloud print pauses it: the head stops and retraces this many pulse ticks with the laser off; the next press resumes, moving forward with the laser off for the resume lead before it re-enables.',
      'The factory values are 2000 and 1950 (about 0.2 s at 10 kHz); 0 to 30000.'
    ]
  },
  jobsize: {
    t: 'Job size',
    d: 'panel/cloud#job-size',
    p: [
      'A cloud print arrives as one compressed file, held in memory and fed to the machine as it plays, so these bound memory rather than how long a job may be.',
      'The service compresses tens to one, which puts an hours-long print at a few megabytes: the defaults warn at 32 MiB and refuse past 128 MiB. 0 lifts either; 0 to 1073741824.'
    ]
  },
  grbl: {
    t: 'Controller',
    d: 'panel/grbl#controller',
    p: [
      'The motion controller speaks standard Grbl 1.1 (grblHAL): point your sender at the connection shown and use its device settings ($$) for the numbered GRBL parameters.',
      'The machine-level tunables on this tab are read by the controller from the shared machine settings.'
    ]
  },
  arming: {
    t: 'Laser arming',
    d: 'panel/grbl#laser-arming',
    p: [
      'A job that fires the laser waits for the physical button (the light comes on) before its first fire; if nobody presses within the button wait the job is refused.',
      'Once armed, the window closes after the disarm grace with the laser off, and the next job asks again. While a job runs the button pauses it, and pressing again resumes.'
    ]
  },
  power_model: {
    t: 'Laser power model',
    d: 'panel/grbl#laser-power-model',
    p: [
      'How a commanded power becomes light. Density keeps every pulse at full power and sets the dose by how many ticks of each period fire, which is what the factory does: every level marks, and a low level is sparse full pulses. Analog sets the PWM duty to the level and fires continuously; the beam is steady, and the response is close to linear above the floor.',
      'Each model has a floor, the bottom of the S range as a percent of full: under density the lowest pulse density that still marks (10 on the bench machine), under analog the duty the tube lases at (16). The controller loads the selected model\'s floor into $35 at every arm, so $35 is not a setting to type. A job selects a model for itself with M101 P0 (analog) or M101 P1 (density) after an M5; it reverts at the end of the program unless the line carries Q1.',
      'The pulse period and shortest pulse shape the density model: one tick is 35.5 us at the 28160 Hz stream rate, the default period of 20 ticks is the factory\'s 1.43 kHz, and a pulse shorter than 3 ticks does not strike this supply. Changes apply at the next job.'
    ]
  },
  lid_policy: {
    t: 'Lid and interlock',
    d: 'panel/grbl#lid-and-interlock',
    p: [
      'The beam is cut by the hardware the instant the lid or the interlock loop opens, whatever is chosen here; this decides what the job does.',
      'Cancel is what the factory firmware does: the head stops, the job ends for the sender, and the head goes back to where the job started, lid open or not; the next job asks for the button again. Hold keeps the stock Grbl door behavior; the button still has to be pressed before the beam can return.'
    ]
  },
  rail: {
    t: 'Motor rail',
    d: 'panel/grbl#motor-rail',
    p: [
      'Off period observed before the 40 V motor rail is re-enabled on a controller takeover, so the stepper drivers always power up from a settled supply. 0 disables.'
    ]
  },
  lamp: {
    t: 'Lid lamp',
    d: 'panel/grbl#lid-lamp',
    p: [
      'Brightness of the lid lamp while the machine is idle; applied immediately, at every start, and after a mode switch. Cloud mode drives the lamp itself while it runs. 0 is dark.'
    ]
  },
  diag: {
    t: 'Cooling system diagnostics',
    d: 'panel/diagnostics#cooling-system',
    p: [
      'Verify (about 10 minutes): one flow check with the pump running and one with it stopped, judged against the configured fault rise. It proves the threshold separates the two on this machine and coolant.',
      'Calibrate (about 30 minutes): three trials per case; measures both bands and recommends a threshold at the midpoint. Run it after replacing the coolant (a different blend carries heat differently), swapping the pump, or a failed verify.'
    ]
  },
  log_levels: {
    t: 'Log levels',
    d: 'panel/logs#log-levels',
    p: [
      'Every ForgeFIRM logger keeps its own directory under /data/log/forgefirm (size-capped, rotated). Levels are cumulative: warning keeps warnings and errors, debug keeps everything, off writes nothing; the two columns are independent.',
      'kernel is the glowforge driver and the rest of the kernel (its levels only filter what the kernel emits); system is everything else (SSH, WiFi, time sync). Changes apply at the next reboot.'
    ]
  },
  syslog: {
    t: 'Remote syslog server',
    d: 'panel/logs#remote-syslog',
    p: [
      'Forwards each logger at its remote level as RFC 5424 syslog. Nothing is sent unless a server is set and at least one logger\'s remote level is on.',
      'An unreachable server never holds up the machine: undeliverable messages are dropped. Applied at the next reboot.'
    ]
  },
  log_viewer: {
    t: 'Log viewer',
    d: 'panel/logs#log-viewer',
    p: ['The tail of one logger. Follow keeps it moving; Refresh fetches once. The viewer keeps the last few hundred kilobytes.']
  },
  log_export: {
    t: 'Export',
    d: 'panel/logs#export',
    p: [
      'A .tar.gz of every logger\'s files plus a system snapshot (firmware version, kernel ring buffer, uptime, memory, disk, processes, effective log levels, settings with secrets masked).',
      'The sanitized bundle is meant for attaching to a public issue report; placeholders keep the same number for the same value, so hosts can still be told apart. The sanitizer removes what it knows and what it can recognize: skim the bundle before posting it. Untick to keep everything for your own use.'
    ]
  },
  slots: {
    t: 'Firmware slots',
    d: 'panel/system#firmware-slots',
    p: [
      'The eMMC carries two firmware slots (the factory A/B scheme); the SD card is the development boot medium. Targets are probed first and unbootable ones are refused.',
      'Returning to ForgeFIRM from factory firmware: sh /data/ffboot -e<slot> at the factory console.'
    ]
  },
  update: {
    t: 'ForgeFIRM update',
    d: 'panel/system#update',
    p: [
      'Downloads are verified against the ForgeFIRM release signing key before anything can be written. Installing writes the selected slot; the running system is untouched until you set next boot and reboot.'
    ]
  },
  install: {
    t: 'Install or restore firmware',
    d: 'panel/system#install',
    p: [
      'Development images upload straight from the browser (dev-key signed, release.sh --dev); unsigned images need an extra confirmation.',
      'After writing, use Set next boot above and reboot to switch.'
    ]
  },
  restore: {
    t: 'Restore factory firmware',
    d: 'panel/system#restore',
    p: ['Restores an archived factory image (md5-verified against the archive manifest first). The Glowforge cloud is never required.']
  },
  wifi: {
    t: 'Wireless',
    d: 'panel/system#wireless',
    p: [
      'Region rules set the WiFi radio\'s allowed channels and transmit power. Automatic follows the country the access point advertises (802.11d), falling back to the most-restrictive world rules; selecting a country pins it regardless of the AP (2.4 GHz channels 12 and 13 and the 5 GHz bands vary by region).',
      'Applied immediately and at every boot. WiFi power save stays off: on a mains-powered machine it only adds latency.'
    ]
  }
};

var helpOpen = null;
function helpContent(h) {
  var d = document.createElement('div'),
    i,
    p,
    a;
  for (i = 0; i < h.p.length; i++) {
    p = document.createElement('p');
    p.textContent = h.p[i];
    d.appendChild(p);
  }
  a = document.createElement('a');
  a.className = 'doclink';
  a.href = DOC_BASE + h.d;
  a.target = '_blank';
  a.rel = 'noopener';
  a.textContent = 'Documentation \u2192';
  d.appendChild(a);
  return d;
}
function closeHelp() {
  if (helpOpen) {
    helpOpen.hide();
    helpOpen = null;
  }
}
function initHelp() {
  var els = document.querySelectorAll('[data-help]'),
    i;
  for (i = 0; i < els.length; i++) {
    (function (el) {
      var h = HELP[el.getAttribute('data-help')];
      if (!h) {
        el.style.display = 'none';
        return;
      }
      var pop = new bootstrap.Popover(el, {
        trigger: 'manual',
        html: true,
        placement: 'auto',
        title: h.t,
        content: helpContent(h),
        container: 'body'
      });
      el.addEventListener('click', function (e) {
        e.preventDefault();
        e.stopPropagation();
        if (helpOpen === pop) {
          closeHelp();
          return;
        }
        closeHelp();
        pop.show();
        helpOpen = pop;
      });
      el.addEventListener('hidden.bs.popover', function () {
        if (helpOpen === pop) helpOpen = null;
      });
    })(els[i]);
  }
  document.addEventListener('click', function (e) {
    var tip = document.querySelector('.popover.show');
    if (helpOpen && !(tip && tip.contains(e.target))) closeHelp();
  });
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') closeHelp();
  });
  window.addEventListener('hashchange', closeHelp);
}

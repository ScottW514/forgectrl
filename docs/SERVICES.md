# Machine-services contract

The shared contract between the three things that touch the Glowforge factory
board's non-motion hardware:

- **forgectrl** (this repo) — the machine-services daemon: web control panel,
  camera service, machine settings, telemetry, diagnostics, logging.
- The **GRBL controller** —
  [grblHAL-glowforge](https://github.com/ScottW514/grblHAL-glowforge), motion +
  laser in GRBL mode.
- The **cloud client** — `gfcloud` /
  [Glowforge-Utilities](https://github.com/ScottW514/Glowforge-Utilities) /
  [python3-gfhardware](https://github.com/ScottW514/python3-gfhardware), motion +
  laser in Glowforge-cloud mode.

Exactly one controller mode is active at a time. Kernel attribute semantics
(ranges, units, feeder contract) are owned by
[kernel-module-glowforge](https://github.com/ScottW514/kernel-module-glowforge)
`UAPI.md`; this document does not restate them except where a conversion or a
polarity is needed by every consumer. Where the two disagree, `UAPI.md` wins for
kernel behavior and this document wins for userspace division of labor.

Sections are tagged **[implemented]** or **[contract]** ([contract] = normative
for work in progress; the interfaces described are the agreed target).

---

## Switches and button [implemented]

All switch inputs surface as **EV_SW events on `/dev/input/event0`** (gpio-keys,
per the device tree). Current state is readable at any time via `EVIOCGSW`; edges
arrive as input events. Bit set = switch active.

| EV_SW code | label (device tree) | meaning when ACTIVE |
|---|---|---|
| 0 | `door1` | door/lid switch 1 closed |
| 1 | `door2` | door/lid switch 2 closed |
| 2 | `button` | the big button is pressed |
| 3 | `doors` | both door switches closed (the series combination the safety chain sees) |
| 4 | `hv_enable` | the safety chain's HV_ENABLE output is asserted (readback) — see below |
| 5 | `interlock` | **remote-interlock loop OPEN** — see below |
| 6 | `interlock_latch` | interlock latch set (LASER_ON blocked in hardware); the kernel sets it while bit 5 reads open and releases it when the loop closes |
| 7 | `head` | head-attention line — **not** a head-present indicator; see below |

**The interlock sense is inverted relative to the door/button switches.** Bit 5
ACTIVE means the regulatory 2-pin remote-interlock loop is *open* (lockout
engaged = laser must not fire). Basic/Plus machines ship the connector
factory-jumpered, so the bit stays inactive there — i.e. satisfied; Pro brings
the connector out for an external lockout chain. Any UI or gate expression must
treat `interlock_ok = !bit5`.

**Bit 4 (`hv_enable`) is a readback, not an input.** It mirrors the safety
chain's HV_ENABLE output (`DOORS_OK · WDOG_ALIVE`, see forgefirm
`docs/SAFETY.md`): inactive at idle, active only while a run feeds the
charge-pump watchdog with the lid closed, and it drops ≈0.45 s after the last
pulse. It is telemetry — `/status` reports it and the control panel shows it —
and nothing gates on it: the beam and the HV supply are already governed by
the chain it reads back.

**Bit 7 (`head`) is not head presence.** Bench-verified: a gen2 head connected
and answering I²C (`head/info` returns id/serial) reads bit 7 INACTIVE. The
line pulses while the head MCU reboots (hence its 60 ms debounce in the device
tree). Detect head presence via the head driver having probed — the
`/sys/glowforge/head/` group exists (`head/hall_sensor` readable) — never this
bit. `/status` `switches.head` reports that presence; the GRBL controller
refuses to arm without it (no lens, no air assist, no beam detector — and the
hardware safety chain does not include head presence).

Who reads what:

- **forgectrl** polls `EVIOCGSW` for `/status` (no exclusive grab).
- **The active controller** reads the button directly (the GRBL arm flow waits
  on code 2; the cloud client's event loop does the same in its mode). Button
  *meaning* is mode-specific by design; the reads stay in-process for latency.
- **The active controller also reads bits 3 and 5 for its own motion gate.**
  In GRBL mode they become the core's safety-door signal — lid open or
  interlock loop open parks a running (or held) job, and a cycle start
  resumes it once closed. While the core is idle, jogging or homing the
  signal is hidden from it, so a lid cycle at idle (loading material) never
  strands the controller in Door; a job started with the lid open parks on
  the first poll. Bit 4 gates nothing (it is a readback of the chain
  itself). The cloud client reads the same bits itself and is unaffected
  (it reports every lid event to the service).
- **No process takes `EVIOCGRAB`** on the device — the cloud client's reader
  included. Exclusivity of button *meaning* comes from mode selection, and a
  grab starves every other reader of events.

---

## Sensor conversions [implemented]

One formula per sensor, stated once. The reference implementation of this whole
section is forgectrl `src/status.c`; the GRBL controller carries the same
coolant conversion for its in-process gate. Consumers must not reintroduce
private variants.

### Coolant temperature (`pic/water_temp_1`, `pic/water_temp_2`)

The factory beta-equation NTC conversion — full derivation, proof, and reference
points in `UAPI.md` (kernel-module-glowforge):

```
F    = 1024 * 1.3                      # ADC steps x gain = 1331.2
R    = 10000 / (F / raw - 1)           # divider resistor 10 kΩ
Rinf = 10000 * exp(-3380 / 298.15)     # R0 = 10 kΩ at 25 C, beta = 3380
degC = 3380 / ln(R / Rinf) - 273.15
```

Higher raw = colder (NTC). Treat `raw <= 0` or `raw >= F` as open/shorted
sensor, not a temperature. `water_temp_1` is **downstream** of the flow-check
heater, `water_temp_2` **upstream**; run/resume ceilings gate on the upstream
sensor. `gfhardware`'s `cooling.py` implements this same curve for the cloud
client.

### Chassis temperature (LM75)

Read via `/sys/class/hwmon`, **resolved by name, never by index**: enumerate
`hwmon*/name` and use the node whose name starts with `lm75`
(`temp1_input`, millidegrees C). hwmon numbering depends on probe order — a
hardcoded `hwmon0` reads the CPU die on current kernels.

### Power-supply and TEC temperatures (`pic/pwr_temp`, `pic/tec_temp`)

`pwr_temp`: best guess `degC = raw * 0.08715 - 21`, **unverified** (positive
slope — this is not the coolant NTC path; see `UAPI.md`). `tec_temp`: conversion
unknown; observed railed at 1023 on some units (likely unpopulated). Report raw
values as raw, never as trusted degrees.

### Fan tachometers

All tach attrs report the **period between pulses**; 0 = stopped/stalled (not
"infinite speed").

| attr | period unit | pulses/rev | RPM |
|---|---|---|---|
| `thermal/tach_exhaust` | ns | 2 | `60e9 / (period * 2)` |
| `thermal/tach_intake_1`, `_2` | ns | 2 | `60e9 / (period * 2)` |
| `head/air_assist_tach` | µs | 8 | `60e6 / (period * 8)` |

`head/purge_air_current` is a raw current reading, qualitative only (observed:
~1 off, ~628 on).

---

## Safety-chain readbacks [implemented]

Laser-safety enforcement is the hardware AND-gate; everything below is
**monitoring** (plus the one kernel lockout latch). Details in `UAPI.md`.

- `cnc/interlock_circuit` — raw safety-chain GPIO bitmask: bit 0 `LASER_ON`,
  1 `LASER_ENABLE`, 2 `BUTTON_LATCH`, 3 `LASER_LATCH`, 4 `INTERLOCK_LATCH_RESET`.
  forgectrl's `/status` reports bit 3 as `laser_locked`.
- `cnc/laser_latch` (write) — the kernel laser lockout: 1 = the SDMA stream
  cannot enable the laser. Owned by the active controller; locked whenever no
  job is in progress, and automatically on `/dev/glowforge` close.
- `cnc/laser_on[_sampled]`, `laser_pgood[_sampled]` — gated-output and
  power-good readbacks for telemetry.

---

## Telemetry [implemented]

`GET /status` on forgectrl (port 8080) is the machine-state source for every
latency-tolerant consumer: the control panel, the cloud client's reporting, and
anything external. It carries motion state, position (homing-anchored kernel
counters), fan RPM, coolant temperatures, pump/TEC state, and the switch map
above.

**Never poll the Grbl TCP socket for status.** A connection there displaces the
sender's session (LightBurn). Controller-side facts reach forgectrl only through
pushed state (`/run` anchor files, and the cooling reports below).

---

## Cooling service [implemented]

The cooling engine — fan/pump/TEC/heater profiles, coolant-flow verification,
over-temp policy — lives in forgectrl (`src/cool.c`) as the single owner of the
thermal hardware, serving both controller modes. Tunables are the `cool_*` keys
in the shared machine settings (re-read at every run start) with `GFCOOL_*` env
overrides. `GET /cool/status` reports the engine state for the UI and bench
tooling. Both controllers are clients of it over the two channels below: they
report job state and enforce the published verdict in-process, and they write
no thermal hardware except through the emergency fallback described under the
verdict file.

The engine also runs the **physical-evidence witnesses** at its 1 Hz tick:

- **Emission**: `cnc/laser_on_sampled` counts the last ~1 s window's emitting
  samples on the gated output of the hardware AND-gate — evidence, not a
  commanded state. Emission with no armed window in the recent past gets the
  hung-controller treatment (`cnc/stop` + `cnc/laser_latch=1`, repeated while
  the evidence persists). Power-good degradation during an armed window is
  warned once per session.
- **Lid IR fire watch**: the four `pic/lid_ir_*` channels are polled every
  tick; each job logs its baseline and peaks (the characterization dataset).
  When `cool_fire_ir_delta` is nonzero, a rise above the run-start baseline on
  any channel sustained for two ticks is a FIRE signal: motion stopped, latch
  locked, verdict `FIRE` with `hold` until the next run session, smoke-clear
  airflow held. The delta ships 0 (watch-only) until the sensors are
  characterized on the bench. `/cool/status` carries the watch state as
  `fire_watch: watch | armed | ALARM`.
- **Telemetry**: `cnc/faults` transitions to nonzero during a run window are
  warned; `pic/hv_current` (the only live HV telemetry) is ranged per job in
  the same log line. `/status` exposes the sampled laser evidence, faults, HV,
  and lid IR values; the panel's latch row is labeled *commanded*, with the
  sensed emission row beside it.

### Job-state reports (controller → forgectrl)

`POST /cool/state`, query or form parameters:

```
mode=idle|run|cooldown & armed=0|1
    [ & air_assist=0..1023 & exhaust=0..65535 & intake=0..65535 ]
```

- **Level-triggered, repeated at ~1 Hz** while the controller runs — not
  edge-triggered events. A lost report self-heals on the next one.
- `armed` = the laser is armed (fire possible). The engine forces the run fan
  profile and flow interrogation whenever `armed` is true, whatever `mode` says.
- The duty parameters are the optional per-job run fan profile (cloud mode
  passes the factory pulse-header duties; GRBL mode omits them →
  configured/factory defaults). Out-of-range values fall back to defaults.
- Silence: if the active controller stops reporting past a timeout (5 s), the
  engine publishes `fire_ok=false` immediately and stands down through the
  normal cooldown path (smoke clear is the right physical behavior for a job
  that died mid-cut), ending at the idle profile (pump on, fans idle, heater
  off). Silence with the armed window open — or with `cnc/state` still
  reading `running` (cloud mode preloads whole jobs, so the ring can play
  for minutes with no live feeder) — is the **hung-controller dead-man**:
  the engine itself writes `cnc/stop` + `cnc/laser_latch=1` (the supervisor
  safes controller *deaths*; this covers *hangs*), and exhaust/intake never
  drop below cooldown duty while the kernel still reports a run in
  progress.

### Verdict file (forgectrl → controllers)

`/run/forgefirm/cooling.state`, JSON, **atomically replaced** (write-temp +
rename) at ~1 Hz and on every verdict change:

```json
{ "seq": 1234, "ts_mono": 5678.9,
  "fire_ok": true, "verdict": "OK",
  "hold": false, "resume_ok": true,
  "reason": "", "down_c": 21.3, "up_c": 21.1 }
```

- `ts_mono` is `CLOCK_MONOTONIC` seconds. **Readers must treat a missing file
  or `ts_mono` older than 2 s as `fire_ok=false, hold=true`.** A body without
  its closing brace is a torn read, not a verdict; an absent `fire_ok`,
  `hold`, or `resume_ok` key takes the fail-safe value (`false`, `true`,
  `false`). The publisher never writes a document longer than its buffer.
- `verdict`: `OK | SUSPECT | FAULT | OVERTEMP | FIRE`. `hold=true` asks the active
  controller for a feed hold; `resume_ok=true` signals recovery below the
  resume gate (auto-resume is the controller's call).
- **Enforcement stays in the controller.** The fire gate and hold/resume
  issuance run in-process in each controller; the verdict file is an input they
  must survive losing. The channel is not fast enough for anything
  safety-critical — the hardware AND-gate is the safety boundary; this is
  equipment protection.
- `fire_ok` additionally requires a fresh job-state report: an armed window
  the engine cannot see never reads `fire_ok=true`. A controller about to
  fire is, by this contract, reporting at 1 Hz.
- While a diagnostic owns the hardware the engine suspends its writes and
  publishes `fire_ok=false, hold=true` until the diagnostic finishes.
- **Emergency fallback:** if the verdict goes stale while the laser is armed,
  the controller (besides gating fire and holding) writes the run fan duties
  directly once — compiled-in factory values, no config dependency — then
  stands down. The one sanctioned exception to single-writer, taken only when
  the single writer is provably absent.

---

## Mode supervision [implemented]

forgectrl owns the controller lifecycle: exactly one of the GRBL controller
or the cloud client runs at a time, spawned as a **direct child** of
forgectrl (the parent-child relationship carries the pulse-device fd under
the broker, and detects controller death the moment it happens). The boot
init scripts do not start controllers — they defer to the supervisor and
remain only as manual emergency stops.

- `GET /mode` →
  `{"mode":"grbl|cloud","controller":"running|stopped|standby|motion-fault","pid":N,"motion":"verified|unverified|fault"}`
- `POST /mode?controller=grbl|cloud` — the live switch: idle-gated (machine
  idle, no diagnostic), stops the active controller (SIGTERM → SIGKILL
  escalation on its process group), persists `controller_mode`, starts the
  other, and waits for its first job-state report to reach the cooling
  engine (slow first report from the cloud client is logged, not fatal).
- `POST /controller/stop` / `POST /controller/start` — the manual emergency
  lever (the controller init scripts route here): stop halts the active
  controller and HOLDS supervision suspended — deliberately **not**
  idle-gated, and the exit safing writes run as always; start resumes
  supervision of the selected mode. A bare `pkill` of a controller would
  just be safed and respawned seconds later.
- **Controller exit safing**: the supervisor safes the machine (`cnc/stop`,
  `cnc/laser_latch=1`) **before it signals a child to stop**, on **every**
  transition out of a running child — unexpected death, mode switch,
  diagnostics suspend, shutdown, the emergency lever — and again immediately
  after any SIGKILL escalation (a killed child runs no cleanup of its own).
  The pre-signal writes are what make the lever immediate: motion
  decelerates and FIRE is severed at the kernel the moment the stop is
  requested, whatever the controller then does with its SIGTERM (the GRBL
  controller treats a termination signal during motion as a `^X`: controlled
  stop, latch relocked, exit). Under the broker a child exit is not a final close of the
  pulse device, so these writes are the safing mechanism; both are harmless
  no-ops when the machine is already idle and latched. Unexpected deaths
  additionally respawn with exponential backoff (1 s → 30 s cap, reset
  after 60 s healthy).
- **Diagnostics takeover** rides the same machinery: suspend (controller
  down, mode unchanged) / resume — the controller that comes back is the
  selected mode's.
- A **busy** controller survives a forgectrl stop: it is left running
  (unmanaged; its own fd carries the dead-man) rather than e-stopping a
  live job. A supervisor that finds an unmanaged controller — at startup,
  or after its own crash-respawn (the init script runs the daemon under a
  respawn wrapper) — stands by and **retakes supervision automatically
  once the machine is idle**; `POST /mode` remains the manual lever.
- **Motion liveness gates the first spawn** of each broker session: the
  supervisor commands a small probe move through its own fd (+X first,
  then back — a cable lives at the end of left travel; laser latched) and
  verifies it physically happened via the head accelerometer. A dead
  verdict runs a rail-off recovery ladder (5/15/30 s — the DRV8825
  drivers can come out of a rail power-up unserviceable and need a true
  power-off to recover); if the ladder fails, controllers stay down and
  `/mode` reports `motion-fault` (retry via `POST /mode`).

## Pulse-device ownership [implemented: broker; rail policy: contract]

`/dev/glowforge` semantics (kernel details in `UAPI.md`): the device is
**exclusive-open** (a second open fails EBUSY), the `flock` on it arms the
kernel dead-man (**final close of the open file description** mid-program =
emergency stop), and every close locks the laser latch. The open itself has
no rail side effect — the 40 V rail moves only on `cnc/enable`/`cnc/disable`
writes. A client that disables and re-enables the rail around its own open
cycles it on every handover, and a fast off→on bounce can leave the supply
folded back (counters run, motors dead), which is why the `rail_settle_s`
off-period guards every standalone takeover and why the broker exists.

**The broker [implemented]:** the forgectrl supervisor opens the device once
(lazily, at the first managed spawn) and holds it for its lifetime, flock'd.
Every controller it spawns inherits the fd, named by **`GF_PULSE_FD`** in the
child environment; exclusive-open then *enforces* that nothing else can open
the device. The GRBL `$H` homing handover needs no socket passing: the driver
forks the homing runner, so the same fd and environment flow down a second
generation. Writers write the pulse ring **directly** through the inherited
fd — the real-time feed path is never proxied.

- A writer that sees `GF_PULSE_FD` must **never close** that fd, never
  self-open the device, and skip its takeover rail settle (the device — and
  the rail's state — is continuous across handovers). The flock re-lock on
  the shared description is a harmless no-op.
- **Exactly one writer:** the supervisor runs exactly one controller and
  safes the machine before any respawn (below). Handovers happen only with
  the kernel idle (the driver's suspend gate, unchanged).
- **Dead-man, re-plumbed [implemented]:** a writer crash no longer produces
  a final close, so the kernel dead-man does not fire on it. The supervisor
  is the dead-man for its writers — `cnc/stop` and `cnc/laser_latch=1` on
  every transition out of a running child, and again after a SIGKILL — and
  the cooling engine is the dead-man for *hangs*: a reporter silent past
  5 s with the window armed or the kernel still running gets the same two
  writes (see the job-state section). The broker fd is opened `O_CLOEXEC`
  and only the controller spawn clears the flag, so helper children (curl,
  fwup, media-ctl, ...) can never hold the device open past an exec and
  defeat the final-close backstop. Below that remain the kernel's own
  backstops (end-of-data forces the lines low; underrun faults; every
  fresh open starts with the flock dead-man disarmed) and the hardware
  AND-gate — the actual safety boundary. forgectrl dying with no writer
  alive closes the description and trips the kernel dead-man; dying while
  a writer runs leaves the writer's dup as the last reference, so the
  writer's own exit becomes the final close — the kernel backstop either
  way.
- **Rail policy [contract]:** `cnc/enable`/`cnc/disable` are forgectrl's
  writes (the liveness ladder, diagnostics under its takeover rules), and
  every deliberate re-enable observes `rail_settle_s`. **The rail stays up
  while the machine is on — there is no idle-rail-off policy**: the stepper
  drivers can come out of any power-up unserviceable, so each cycle is a
  fresh gamble and the cheapest policy is not to cycle. With a brokered fd
  in play no client drops the rail on a handback or a takeover, and takeover
  settles are skipped because the rail never went down (an emergency halt
  may still disable it deliberately). The GRBL controller's `cnc/enable` at
  init and at homing resume are the one residual controller write:
  idempotent against a rail that is already up (listed in the ownership
  table below).

**Watchdog scope:** the i.MX6 hardware watchdog is a boot/system watchdog,
not a laser-safety watchdog — nothing ties `/dev/watchdog` to controller
liveness, motion liveness, or the armed state, and it must not be mistaken
for a beam stop (a boot-enabled WDT that userspace never opens is petted by
the kernel indefinitely). The fast beam-stop path on a feeder stall is the
ring-drain chain: the ring runs dry → the SDMA script forces the FIRE and
step lines low in the same tick → the driver leaves the running state → the
charge pump self-terminates on its next 200 ms tick and the HV watchdog
disarms the chain. Cloud mode preloads whole jobs, so its ring does not
drain on a feeder stall — that residual is covered by the cooling engine's
hung-controller dead-man above.

## Logging [implemented]

One transport, one writer. Every ForgeFIRM process emits through the
system syslog socket (`/dev/log`) and **rsyslog is the only file writer**:
nobody opens its own log file, and nothing of ours is written anywhere under
the factory's `/data/log/*` directories or as `/data/*.log`.

**Emitters.** forgectrl and the grblHAL driver use the shared `fflog`
emitter (`src/fflog.[ch]`, vendored verbatim into grblHAL-glowforge; keep
the copies identical). It differs from glibc `syslog(3)` in one deliberate
way: the socket is non-blocking and a message that cannot be queued is
dropped and counted, never waited for — a unix datagram socket exerts
backpressure on its sender, so a stalled log daemon could otherwise park a
controller thread. The RT feeder thread never logs except a fault. The
Python apps (gfcloud, gfhome) use `logging.handlers.SysLogHandler` on a
non-blocking socket with the same drop semantics. Stray stdout/stderr of a
controller (an interpreter traceback, a library message) reaches syslog
through a `logger` relay the supervisor spawns per controller under the
controller's program name (double-forked, outlives the daemon while the
controller does); the daemon's own stray output flows through a fifo relay
in its init script.

**Loggers and tree.** `LOGS_ROOT = /data/log/forgefirm/<logger>/<logger>.log`
plus rotated `.N.gz`:

| Logger | Selector | Emitter |
|---|---|---|
| `forgectrl` | `programname == forgectrl` | fflog (+ fifo relay) |
| `grblhal` | `programname == grblhal` | fflog (+ relay) |
| `gfcloud` | `programname == gfcloud` | SysLogHandler (+ relay) |
| `gfhome` | `programname == gfhome` | SysLogHandler; stray output rides the grblhal relay (it is the controller's child) |
| `kernel` | facility `kern` (imklog) | printk — levels only filter |
| `system` | everything else | sshd, ntpd, wpa_supplicant, init scripts via `logger` |

Line format (`ff_line`): `2026-08-15T16:35:44.123456-04:00 forgectrl[512] INFO super: started grbl controller (pid 3084)`.

**Levels.** Per logger, two independent settings in `/data/forgefirm.conf`:
`log_<logger>_disk` (default `info`) and `log_<logger>_remote` (default
`off`), each `off|error|warning|notice|info|debug`; plus `syslog_server`,
`syslog_port` (514) and `syslog_proto` (`udp`|`tcp`) for the remote target
(RFC 5424 forwarding, per-action queue that discards when full — an
unreachable server never stalls the disk writers). Rule: **a process emits
at the more verbose of its two levels** and rsyslog filters per destination
(`fflog_init` reads both keys itself; the kernel is filter-only). Python has
no `notice`; its loggers emit INFO for both `info` and `notice`.

**Applied at reboot, by design.** `forgectrl --render-syslog` renders the
rules into `/data/forgefirm/rsyslog-forgefirm.conf` (included by
`/etc/rsyslog.conf`) and records the levels in force in
`/var/run/forgefirm-loglevels`; the `forgefirm-logging` init script runs it
before rsyslog starts and each process reads its own level at start. There
is no live re-level path; the panel shows configured vs. effective and
offers the reboot. Rotation: logrotate, size-capped per file, `HUP` to
rsyslog (never `copytruncate`), driven by the same init script at boot and
hourly.

**Panel / API.** `GET /logs`, `GET /logs/tail`, `POST /logs/export`
(token-gated; see the README). The export is a `tar.gz` of the tree plus a
system snapshot; sanitized by default for public issue reports. The
sanitizer (`src/sanitize.c`) is two-layer — exact known values (serial,
hostname, cloud credentials, panel token, WiFi SSID/PSK/identity) then
pattern classes (bearer/basic credentials, JWTs, key=value secrets, e-mail,
MAC, IPv4/IPv6 with loopback kept, hex >= 32, base64-like >= 40) — with
stable per-value placeholders and an idempotent output; `tests/sanitize_test.c`
runs in CI and is the regression gate for both leaks and over-redaction.

**Rules for new code.** No `fprintf(stderr)`/`printf` for logging in the C
daemons — `fflog(LOG_x, ...)`, message without a program prefix (rsyslog
tags it) and without a trailing newline. Never log a credential, token, or
password value; the sanitizer is the second line, not the first. Never log
from the RT feeder path except a fault. New loggers are added to
`logs_names[]` (forgectrl) plus a setting pair; new secrets to
`load_known()`.

---

---

## Clocks [rule]

The board has no RTC: wall-clock time is whatever NTP last set and steps by
years across a cold boot (`ntp.conf` carries `tinker panic 0` so it may).
Every job, timeout, deadline, and freshness comparison in this stack — the
verdict `ts_mono` and its 2 s freshness, the report cadences, the button wait
and disarm grace, the dead-man silences, the supervisor's stop deadlines and
respawn backoff, the liveness ladder — is on `CLOCK_MONOTONIC`. Wall-clock
time (`time()`, `CLOCK_REALTIME`) is for display and log stamps only. **Never
put a safety, job, or timeout decision on the wall clock.**

---

## Hardware ownership [contract]

Single-writer rule: each attr group has exactly one writing process per mode.
Readers are unrestricted.

| Hardware | GRBL mode | Cloud mode | Diagnostics |
|---|---|---|---|
| `thermal/*` fans/pump/TEC/heater, `head/air_assist_pwm`, `head/purge_air` | forgectrl engine (+ the controller's stale-verdict fallback) | forgectrl engine (+ fallback) | forgectrl runner (engine suspended, fire blocked) |
| `cnc/*` motion, `/dev/glowforge` ring | GRBL controller, through the brokered fd | cloud client, through the brokered fd | — (controller suspended) |
| `cnc/enable` / `cnc/disable` (40 V rail) | forgectrl; the controller's enable-at-init is the one residual write (rail policy above) | forgectrl | forgectrl |
| `cnc/laser_latch` | GRBL controller (locked by forgectrl across handovers and on writer death) | cloud client (same) | — |
| Button LEDs (`/sys/class/leds/button_led_*`) | GRBL controller (arm flow) | cloud client | — |
| Head/lid illumination (camera lamps) | forgectrl (`lamp` on snapshot) | forgectrl | forgectrl |
| Cameras (V4L2, MIPI mux) | forgectrl | forgectrl | forgectrl |
| `/data/forgefirm.conf` settings | read (re-read per `$H` / run start) | read | read; forgectrl writes (409 while busy) |
| `/run/grblhal.homed` anchor | GRBL controller writes | — | — |
| `/run/forgefirm/cooling.state` | forgectrl writes, controllers read | forgectrl writes, controllers read | forgectrl writes |
| `/data/log/forgefirm/**` log files | rsyslog writes; every process emits to `/dev/log` only | same | same |

On a kernel dead-man trip (and on module removal) the kernel itself
de-energizes the heat sources — loop heater and TEC — and touches nothing
else: pump, exhaust/intake, and head airflow stay with the engine, which
keeps circulation and airflow running over a hot tube.

Diagnostics ownership (forgectrl stops the motion controller, marker file
`/run/forgefirm-diag.active`, recovery on next start) is described in the
diagnostics section of the ForgeFIRM bring-up runbook.

---

## Verification status

Checked against the device tree (`glowforge.dts` gpio-keys node),
kernel-module-glowforge `UAPI.md`, and a live-board spot-check:

- Attr inventory: every attr named in this document exists under
  `/sys/glowforge/{cnc,head,pic,thermal}`.
- hwmon: `hwmon0` = `imx_thermal_zone` (CPU die), `hwmon1` = `lm75b` —
  confirming resolve-by-name is required.
- Switch bits 0–3, 5, 6 verified against physical state; bits 4 and 7
  corrected from live readings (see the switch section).
- Sensor conversions verified against live raws: coolant beta-3380 output
  matches the `/status` values; `tec_temp` railed at 1023 as noted; tach
  periods produce plausible RPM in all three unit/pole variants.

The channels and ownership rules above are drilled on hardware, operator
present:

- **Cooling channels:** idle/run/cooldown postures hit the exact factory
  duties; a silent controller blocks fire immediately and stands down through
  smoke; an engine killed mid-flood leaves the run fans held, the heater
  dropped by the client, and the sender warned, with the posture rebuilt from
  level-triggered reports within ~2 s of restart; over-temp hold and
  auto-resume inside a running cycle.
- **Armed windows:** the fallback rewrites the run duties when the verdict
  goes stale while armed; a controller killed mid-fire drops FIRE with the
  kernel ring's in-flight bytes (tens to ~170 ms, always riding real motion)
  and the supervisor relocks the latch in the same window.
- **Supervision and broker:** live mode switches both ways, crash respawn
  after safing, diagnostics suspend/resume returning the selected mode, a
  busy controller surviving a forgectrl stop and being retaken at idle, and
  `$H` homing handovers with no device open/close and no rail movement.
- **Cloud mode** runs the full stack as an engine client over a multi-hour
  signed-in session, including reconnects and clean stops.

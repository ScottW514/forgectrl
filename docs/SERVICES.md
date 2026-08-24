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
  Outside the arm wait the button is the job pause/resume toggle in both
  modes: in GRBL mode a press while running is a feed hold and a press while
  held is a cycle start; in cloud mode a press is a controlled stop plus a
  laser-off backtrack and the next press resumes with a laser-off lead (the
  `cloud_pause_backtrack_ticks` / `cloud_resume_lead_ticks` settings, factory
  2000 / 1950). A held button has no further meaning during a job.
- **The active controller also reads bits 3 and 5 for its own motion gate.**
  In GRBL mode they become the core's safety-door signal, and what happens
  next is the `lid_policy` setting: `cancel` (default, the factory's
  behavior) — the job parks with a planned deceleration and is then
  cancelled: armed window closed, reason reported, a soft reset ends the
  sender's stream with the position kept (no alarm), and the head returns
  on its own to where the job started, lid open or not; `hold` — the stock
  door hold, a cycle start resumes it once closed. During the arm wait
  (button lit) either opening cancels the job outright under both
  policies. While the core is idle, jogging or homing — and during the
  return-to-start motion after a cancel — the signal is hidden from it, so
  a lid cycle at idle (loading material) never strands the controller in
  Door; a job started with the lid open parks (and cancels) on the first
  poll. Bit 4 gates nothing (it is a readback of the chain itself).
  The cloud client reads the same bits itself: lid or interlock open during
  a print or motion (or the pre-print button wait) cancels the job with a
  controlled stop and the print parks with the lid open; a hunt and the
  park itself ignore the lid; the button pauses/resumes a print. It reports
  every lid event to the service.
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
counters), fan RPM, coolant temperatures, pump/TEC state, the board
temperatures (`temps`: `chassis_c` from the LM75, `soc_c` from the i.MX6
on-die monitor, `supply_raw` from `pic/pwr_temp` as the count, and
`soc_throttle`, the kernel's CPU-frequency cooling state, 0 at full speed;
each `null` when absent; watched, not gated), the SoC load (`sys`:
`cpu_pct`, busy percent from `/proc/stat` over the interval since the
previous status read, `null` on the first read; `mem_pct`, used percent
from `MemTotal` against `MemAvailable`), and the switch map above.

**Never poll the Grbl TCP socket for status.** A connection there displaces the
sender's session (LightBurn). Controller-side facts reach forgectrl only through
pushed state (`/run` anchor files, and the cooling reports below).

---

## Cooling service [implemented]

The cooling engine (fan/pump/TEC/heater profiles, coolant-flow verification,
over-temp policy) lives in forgectrl (`src/cool.c`) as the single owner of the
thermal hardware, serving both controller modes. Tunables are the `cool_*` keys
in the shared machine settings (re-read at every run start) with `GFCOOL_*` env
overrides. `GET /cool/status` reports the engine state for the UI and bench
tooling. Both controllers are clients of it over the two channels below: they
report job state and enforce the published verdict in-process, and they write
no thermal hardware except through the emergency fallback described under the
verdict file.

### Gate settings: range, band, off end

A gate is a comparison the engine makes against a setting. Every gate setting
is a plain number on the Machine tab, and there is no separate switch for any
gate: the one table in `src/gates.c` gives each its **legal range** (wide on
purpose), its **recommended band** (the shipped default with the margin one
machine's loop was seen to need), and its **off end**, the end of the legal
range at which the gate never trips. A value outside the band is legal and
warned about; a value at the off end is legal and reported as the gate being
off. The validators, the engine, the settings reply and the panel all read
that table.

| Key | Gate | Default | Legal | Recommended | Off at |
|---|---|---|---|---|---|
| `cool_temp_max` | `coolant_max` (the run ceiling) | 33 C | 5 to 60 C | 25 to 38 C | 60 C |
| `cool_temp_resume` | (follows the ceiling; kept below it) | 31 C | 5 to 59 C | 20 to 36 C | never |
| `cool_temp_critical_c` | `coolant_critical` (the fail tier above the ceiling; kept above it while the ceiling gates) | 38 C | 6 to 70 C | 36 to 45 C | 70 C |
| `cool_flow_check_s` | `flow` (flow verification) | 50 s | 0 to 300 s | 30 to 120 s | 0 |
| `cool_flow_rise` | (tunes `flow`; set from flow calibrate) | 14.4 C | 1 to 40 C | 8 to 16 C | never |
| `cool_tach_exhaust_min_rpm` | `exhaust` | 6400 rpm | 0 to 20000 | 5800 to 7000 | 0 |
| `cool_tach_intake_min_rpm` | `intake` (either tach) | 2290 rpm | 0 to 20000 | 2100 to 2500 | 0 |
| `cool_tach_air_assist_min_rpm` | `air_assist` | 6000 rpm | 0 to 30000 | 5500 to 6600 | 0 |
| `cool_purge_min_current` | `purge` (current, raw) | 300 | 0 to 1023 | 150 to 500 | 0 |
| `cool_fan_grace_s` | (the spin-up window, no gate) | 15 s | 0 to 120 s | 5 to 30 s | never |

What an off gate does: the engine skips the comparison (no verdict, no hold
from it) and keeps measuring. With `coolant_max` off, the first reading in a
run session over the shipped default is logged once as what the gate would
have done; with a fan floor off, the first reading under the shipped default
likewise; with `flow` off there is no heater interrogation at all and the
run start says so. A header limit never overrules an off gate. What an off gate is not: a way to reach anything that is
not a thermal gate. The hardware chain, the laser latch, the emission witness,
the lid IR fire watch, the controller-silence dead-man and the motion-liveness
gate are not numbers on the Machine tab.

Where it shows: at every run start the engine logs one line per gate setting
(`cool: gate coolant_max OFF: cool_temp_max = 60 ...`, `cool: cool_temp_max =
45 is outside the recommended 25 to 38 ...`, or the plain value); `GET
/settings` carries a `gates` object (per key: `gate`, `def`, `lo`, `hi`,
`band`, `off`, `value`, `state` of `ok | warn | off`, classified from the
stored value); `GET /cool/status` and `GET /status` carry `gates_off`, the
gate names at their off end as the engine resolved them at the last run
start. The panel warns beside a field outside its band, says "this gate is
OFF" at the off end, and shows a standing banner on the Status tab while any
gate is off. None of it is reported to the cloud.

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
- **Airflow gates** (`src/airflow.c`): while the run profile is applied,
  the exhaust, both intakes and the air assist are held to a floor by
  tachometer and the purge-air fan by its current, each floor the effective
  one (a header's tach window can raise it for a job). The floors were
  measured at the configured run duties, so a fan is judged at that
  operating point: always while the laser is armed, when a job's own
  profile (`POST /cool/state` duties) may raise a fan but never lower it
  below the configured run duty; unarmed, whenever the fan is commanded at
  or above the run duty. A fan a job runs slower unarmed (a cloud hunt:
  exhaust and intake duty 0, air assist at idle) is measured and published
  as `unjudged`. The purge fan has no duty and is judged in every run. A
  grace of `cool_fan_grace_s` runs from the moment the run profile is
  written, and again when a fan is commanded faster mid-run (the armed
  window opening raises a lowered fan), and nothing counts inside it; three
  consecutive 1 Hz ticks under the floor
  trip the gate and a reading at or above it clears the count. A trip is a
  fault for the rest of the run session: verdict `AIRFLOW`, `fire_ok=false`,
  `hold=true`, no `resume_ok` in that session, the fans held at run duty,
  the reason naming the fan, the reading and the floor; the fault ends with
  the session (logged), and the next session judges every fan afresh.
  Outside a run the gates read `idle`. `/cool/status` carries `fan_gates` (per fan:
  `reading`, `floor`, `state` of `grace | ok | under | TRIPPED | off | unjudged | idle`).
- **Telemetry**: `cnc/faults` transitions to nonzero during a run window are
  warned; `pic/hv_current` (the only live HV telemetry) is ranged per job in
  the same log line. The chassis, SoC die and supply temperatures are
  ranged over every run session and named once at its end (`cool: temps
  this job: chassis 24.1..31.8 C, soc 42.8..61.0 C, supply raw 401..455`,
  with `CPU THROTTLED for N s` appended when the kernel's thermal governor
  throttled the CPU during the job); no gate stands behind any of them
  until that record says where one belongs. The SoC guards itself: the
  i.MX6 thermal zone throttles the CPU (and the GPU) at its passive trip
  and powers the board off at its critical one; the engine names a
  throttle when it starts and when it ends (`SoC throttled: CPU cooling
  state N (die T C)`). `/status` exposes the sampled laser evidence, faults, HV,
  and lid IR values; the panel's latch row is labeled *commanded*, with the
  sensed emission row beside it.

### Job-state reports (controller → forgectrl)

`POST /cool/state`, query or form parameters:

```
mode=idle|run|cooldown & armed=0|1
    [ & air_assist=0..1023 & exhaust=0..65535 & intake=0..65535 ]
    [ & coolant_max_c=<C> & coolant_min_c=<C>
      & exhaust_min_rpm=<rpm> & intake_min_rpm=<rpm> & air_assist_min_rpm=<rpm> ]
```

- **Level-triggered, repeated at ~1 Hz** while the controller runs — not
  edge-triggered events. A lost report self-heals on the next one.
- `armed` = the laser is armed (fire possible). The engine forces the run fan
  profile and flow interrogation whenever `armed` is true, whatever `mode` says.
- The duty parameters are the optional per-job run fan profile (cloud mode
  passes the factory pulse-header duties; GRBL mode omits them →
  configured/factory defaults). Out-of-range values fall back to defaults.
- The limit parameters are the job's envelope, the pulse header's
  `CMrx`/`CMrn` (millidegrees, sent as degrees) and the tach maximum
  periods `EFrx`/`IFrx`/`AArx` (sent as the minimum speed they mean in
  the kernel's units: ns at 2 pulses/rev for exhaust and intake, us at 8
  for the air assist). The client drops a tag that is absent, a sentinel
  (0, 1023, the signed extremes, the unsigned rail) or absurd. **Tighten
  only:** the engine resolves each limit as the stricter of its own
  configured value and the header's, a ceiling only ever comes down and a
  floor only ever goes up, a looser header value is named once per run
  session and ignored, and a gate the operator set to its off end stays
  off whatever a header says. When a header tightens the coolant ceiling
  the resume gate follows it down by the configured gap; the fan floors
  feed the airflow gates; the coolant critical line is local only, since
  no header carries one. The effective set is resolved, logged and
  published. Level-triggered like the duties:
  the limits ride every report while the job is loaded and leave with it.
  The engine logs `cool: effective limits: ...` whenever the effective set
  changes and carries it in `GET /cool/status` as `limits` (`coolant_max_c`,
  `coolant_resume_c`, `coolant_source` of `local | header`, and the floors).
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
- `verdict`: `OK | SUSPECT | FAULT | OVERTEMP | CRITICAL | AIRFLOW | FIRE`.
  `hold=true` asks the active controller for a feed hold; `resume_ok=true`
  signals recovery below the resume gate (auto-resume is the controller's
  call). `OVERTEMP` is the pause tier (the coolant over `cool_temp_max`,
  back in service under the resume gate). `CRITICAL` (the coolant at or
  over `cool_temp_critical_c` in a run session), `AIRFLOW` and `FIRE` are
  the fail tier: they hold for the rest of the run session and never offer
  a resume in it; `CRITICAL` and `AIRFLOW` end with the session (the
  ceiling's pause tier keeps holding while the loop is hot), `FIRE` holds
  until the next one starts. Controllers key on the flags, not the name; an
  unknown name with `hold=true` holds.
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
  once the machine is idle**: it stops the unmanaged controller, holds the
  pulse device, re-probes motion, and starts a supervised controller of the
  selected mode (a new process — the old one's inherited fd cannot be
  adopted); `POST /mode` remains the manual lever.
- **Motion liveness gates the first spawn** of each broker session: the
  supervisor commands a small probe move through its own fd (+X first,
  then back — a cable lives at the end of left travel; laser latched, no
  axis masked, the run-current step settled before sampling) and
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

## Camera privacy gate [implemented]

**Neither camera captures unless the lid is closed.** The lid camera faces the
room once the lid is raised, and in cloud mode the capture request comes from a
remote service, so the enclosure being shut is the precondition for any image.

The signal is EV_SW bit 3 (`doors`, the series combination both lid switches
feed - the same one the hardware safety chain uses); `machine_lid_closed()` in
`src/status.c` reads it and **fails closed**, so an unreadable lid refuses
capture. There is no setting to disable the gate.

Enforced in two places, because two processes can reach a sensor:

| Owner | Covers | Behavior |
|---|---|---|
| forgectrl `src/cam.c` | the panel, `/cam/stream`, `/cam/snapshot`, the mjpg-streamer aliases, LightBurn, and the cloud client's normal path | refuses to start capture, refuses stream and snapshot up front (HTTP 409), and re-checks every frame so a lid opened mid-capture tears the pipeline down |
| `gfhardware.cam.capture()` | the cloud client's direct-V4L2 fallback when forgectrl is unreachable, and the capture utility | raises `gfhardware.cam.LidOpen` before configuring the pipeline or touching a lamp |

Both check before any side effect, so a refused capture leaves the lamps and
the media graph as they were. `/cam/status` reports `capture_allowed` (the live
lid reading) and `stopped_by_lid` (the last capture ended because the lid
opened rather than going idle).

Consequence for cloud mode: the factory ran focus hunts with the lid open, and
a hunt includes a head capture. Those captures are now refused, and the action
runner reports the action as failed rather than leaving the service waiting.

---

## Camera sensors [implemented; OV8856 untested]

Two sensors ship on the shared `camera@36` node, and the capture path follows
whichever driver bound rather than assuming one. Clients must take the frame
size from `GET /cam/status` (`sensor`, `snapshot`, `stream`) instead of
hard-coding it.

| Sensor | Machines | Capture mode | Media bus | Capture pixel format | Snapshot | Stream |
|---|---|---|---|---|---|---|
| OV5648 | 5 MP (Basic / Plus / Pro) | 2592x1944 | `SBGGR8_1X8` | `SBGGR8`, 1 byte/sample | 2592x1944 | 1296x972 |
| OV8856 | 8 MP ("HD") | 3264x2448 | `SBGGR8_1X8` | `SBGGR8`, 1 byte/sample | 3264x2448 | 1632x1224 |

Both are read as 8-bit BGGR, so one demosaic and one capture word serve both
and the only thing that changes with the sensor is the geometry. Both widths
are multiples of 32, so both take the NEON superpixel path.

The OV8856's stock RAW10 full-resolution mode is *not* reachable on this SoC:
it runs the link at 1.44 Gbps/lane and the i.MX6 CSI-2 D-PHY stops at 1 Gbps,
so `imx6-mipi-csi2` refuses to program the receiver. Full resolution comes
instead from a RAW8 mode carried at 360 MHz, added to the driver by the
BSP - 8-bit samples need half the link rate for the same frame, and at
180 Mpx/s the mode runs 15 fps. Its control set still differs from the
OV5648's: exposure counts whole lines and is capped by the frame length
(2482), gain is `analogue_gain` with 128 = 1x, and it publishes no
auto-exposure, auto-gain or white-balance controls, so white balance is
uncorrected. The exposure and gain defaults are the OV5648 values translated
into those units and have never been measured on real 8 MP hardware.

### Stream encoding and demosaic

One capture serves every consumer, but the stream conversion has layered
implementations, each probed at runtime and each falling back to the next:

| Stage | First choice | Fallback | Switch |
|---|---|---|---|
| Demosaic (stream) | GC880 GPU fragment shaders (`src/gpu_debayer.c`): capture dmabuf in, encoder dmabuf out, CPU untouched | NEON superpixel (`src/debayer.c`), then scalar | `FORGECTRL_NO_GPU`, `FORGECTRL_NO_NEON` |
| MJPEG frames | CODA960 JPEG unit (`src/vpu_jpeg.c`) | libjpeg | `FORGECTRL_NO_VPU` |
| H.264 stream (`/cam/h264`, fragmented MP4 via `src/vpu_h264.c` + `src/mp4mux.c`) | CODA960 BIT processor | none: the endpoint answers 503 and MJPEG remains | `FORGECTRL_NO_H264` |
| fps cap | CSI hardware frame skip (frames dropped before DMA) | software pacing in the worker | `FORGECTRL_NO_HW_SKIP` |

The GPU path loads Mesa with `dlopen` (no build-time GL dependency); an image
without Mesa, a kernel without etnaviv, or any refused probe lands on the NEON
path with the reason logged once. The two CODA engines are independent, so
MJPEG and H.264 clients can be served concurrently; both encoder OUTPUT
buffers are exported as dmabufs and the GPU renders into them directly.
Snapshots always use the CPU bilinear demosaic. `/cam/status` reports the
active choices (`convert`, `encoder`, `hw_fps_skip`, `h264`). H.264 viewers
count toward engine arbitration and idle exactly like MJPEG viewers, and a
joining H.264 viewer forces an IDR so it can start decoding immediately.

### Frame health

The capture queue flags a buffer `V4L2_BUF_FLAG_ERROR` when the frame in it is
short, torn, or arrived after the receiver lost CSI-2 sync. Such a frame is
never demosaiced - a snapshot built from one is a corrupt image presented as a
picture of the bed. The engine drops it, and escalates if they persist: four
consecutive errored frames cycle the capture queue (which re-synchronizes the
receiver), and three cycles with no usable frame between them stop the engine,
so clients reconnect and the whole pipeline setup runs again. The ladder is
`src/camhealth.c`, covered by a host test.

`GET /cam/status` carries the running totals since the daemon started:

```json
"health": { "captured": 41230, "corrupt": 0, "restarts": 0 }
```

A nonzero `corrupt` on a machine that is otherwise working is a real signal - a
marginal camera ribbon, a mistimed D-PHY - even though those frames never
reached a client.

Two frames are also dropped after every stream start: they were already in
flight while the sensor was being programmed, so they predate its exposure
settling and a snapshot could otherwise be handed one.

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
| Head/lid illumination (camera lamps) | forgectrl (`lamp` on snapshot); the lid lamp's idle level is the `lid_lamp_idle` setting (0-255, default 236), asserted at daemon start, on a settings change, and at every controller spawn | the cloud client drives the lid lamp while it runs (its `LLvl`); forgectrl re-asserts the idle level at the next spawn | forgectrl |
| Cameras (V4L2, MIPI mux) | forgectrl; capture only with the lid closed (privacy gate above) | forgectrl, same gate — including the cloud client's direct-capture fallback | forgectrl, same gate |
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
- The gates, on the bench: the coolant ceiling trips and turns off by value
  (`cooling.gate-off`), every fan floor trips on an unmeetable floor and on
  an unplugged exhaust fan (`cooling.fan-gate-trips`, the drill), a hunt
  with its fans off is measured and not judged (`cloud.mode-switch`), the
  critical line faults over the ceiling's pause on a genuinely rising loop
  (`cooling.critical-tier`, the drill), and the header's limits reach the
  engine on a live print (`cloud.pause-resume`). `pwr_temp` stays a raw
  count by decision: its heatsink cannot be reached with a thermometer
  while the machine runs, so the conversion is not verified here, and it
  is never published as degrees.

The channels and ownership rules above are drilled on hardware, operator
present:

- **Cooling channels:** idle/run/cooldown postures hit the exact factory
  duties; a silent controller blocks fire immediately and stands down through
  smoke; an engine killed mid-flood leaves the run fans held, the heater
  dropped by the client, and the sender warned, with the posture rebuilt from
  level-triggered reports within ~2 s of restart; over-temp hold and
  auto-resume inside a running cycle; a ceiling set just over its legal
  minimum trips at the next run start and a ceiling at its top turns the gate
  off with `gates_off` and the run-start line saying so
  (`cooling.gate-off` in the acceptance catalog); an exhaust floor no fan
  reaches trips `AIRFLOW` after the grace and three ticks, a purge current
  floor at the rail likewise, and a floor of zero reads off
  (`cooling.fan-gate-trips`).
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

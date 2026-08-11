# Machine-services contract

The shared contract between the three things that touch the Glowforge factory
board's non-motion hardware:

- **forgectrl** (this repo) — the machine-services daemon: web control panel,
  camera service, machine settings, telemetry, diagnostics.
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
| 4 | `estop` | e-stop sense line high — the **normal** state; see below |
| 5 | `interlock` | **remote-interlock loop OPEN** — see below |
| 6 | `interlock_latch` | interlock latch tripped |
| 7 | `head` | head-attention line — **not** a head-present indicator; see below |

**The interlock sense is inverted relative to the door/button switches.** Bit 5
ACTIVE means the regulatory 2-pin remote-interlock loop is *open* (lockout
engaged = laser must not fire). Basic/Plus machines ship the connector
factory-jumpered, so the bit stays inactive there — i.e. satisfied; Pro brings
the connector out for an external lockout chain. Any UI or gate expression must
treat `interlock_ok = !bit5`.

**Bit 4 (`estop`) reads ACTIVE on a healthy, fireable machine** — it is a sense
line whose resting state is high, not an "asserted = stop" input (no Glowforge
model has a user e-stop, and the hardware fire chain carries no e-stop term).
Treat inactive as the abnormal state (line low).

**Bit 7 (`head`) is not head presence.** Bench-verified: a gen2 head connected
and answering I²C (`head/info` returns id/serial) reads bit 7 INACTIVE. The
line pulses while the head MCU reboots (hence its 60 ms debounce in the device
tree). Detect head presence via `head/info` I²C liveness, never this bit.

Who reads what:

- **forgectrl** polls `EVIOCGSW` for `/status` (no exclusive grab).
- **The active controller** reads the button directly (the GRBL arm flow waits
  on code 2; the cloud client's event loop does the same in its mode). Button
  *meaning* is mode-specific by design; the reads stay in-process for latency.
- **No process takes `EVIOCGRAB`** on the device. Exclusivity of button
  *meaning* comes from mode selection, and a grab starves every other reader of
  events. (The cloud client's grab is scheduled for removal.)

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
sensor. The legacy linear approximation still present in `gfhardware`'s
`cooling.py` does not match this curve and is slated for replacement — do not
copy it into new code.

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

## Cooling service [contract]

The cooling engine — fan/pump/TEC/heater profiles, coolant-flow verification,
over-temp policy — is moving into forgectrl as the single owner of the thermal
hardware, serving both controller modes. The engine currently lives in the GRBL
controller (`glowforge_cooling.c`); this section is the interface it moves
behind. Tunables remain the `cool_*` keys in the shared machine settings.

### Job-state reports (controller → forgectrl)

`POST /cool/state`, JSON body:

```json
{ "mode": "idle" | "run" | "cooldown",
  "armed": false,
  "profile": { "air_assist": 1023, "exhaust": 65535, "intake": 43278 } }
```

- **Level-triggered, repeated at ~1 Hz** while the controller runs — not
  edge-triggered events. A lost report self-heals on the next one.
- `armed` = the laser is armed (fire possible). The engine forces the run fan
  profile and flow interrogation whenever `armed` is true, whatever `mode` says.
- `profile` is optional (cloud mode passes the factory pulse-header duties;
  GRBL mode omits it → configured/factory defaults).
- Silence: if the active controller stops reporting past a timeout, the engine
  reverts to the idle profile (pump on, fans idle, heater off) and publishes
  `fire_ok=false`.

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
  or `ts_mono` older than 2 s as `fire_ok=false, hold=true`.**
- `verdict`: `OK | SUSPECT | FAULT | OVERTEMP`. `hold=true` asks the active
  controller for a feed hold; `resume_ok=true` signals recovery below the
  resume gate (auto-resume is the controller's call).
- **Enforcement stays in the controller.** The fire gate and hold/resume
  issuance run in-process in each controller; the verdict file is an input they
  must survive losing. The channel is not fast enough for anything
  safety-critical — the hardware AND-gate is the safety boundary; this is
  equipment protection.
- **Emergency fallback:** if the verdict goes stale while the laser is armed,
  the controller (besides gating fire and holding) writes the run fan duties
  directly once — compiled-in factory values, no config dependency — then
  stands down. The one sanctioned exception to single-writer, taken only when
  the single writer is provably absent.

---

## Pulse-device ownership [contract]

`/dev/glowforge` carries three side effects that make its open/close lifecycle a
machine-behavior concern, not a file-handling detail (kernel semantics in
`UAPI.md`): open powers the 40 V motor rail, final close is the kernel dead-man
(mid-program close = emergency stop, laser latch locked), and the `flock` on it
is what makes motion ownership exclusive.

**Today [implemented]:** each controller opens the device itself and holds it
for its process lifetime; mode switches and the GRBL `$H` homing session hand
the device over by close-and-reopen. Every handover therefore cycles the 40 V
rail. A fast off→on cycle can leave the rail folded back — playback and
position counters then run normally while the motors produce no torque — so
every device takeover currently begins with a deliberate rail-off settle
(`rail_settle_s`, default 2.5 s).

**Target:** forgectrl owns the device. It opens `/dev/glowforge` once at start
and holds it for its lifetime; the underlying open file never closes across
mode switches or homing handovers, so the rail never cycles as a side effect.

- **Access:** the active motion writer receives a duplicate of forgectrl's fd —
  inherited at spawn (forgectrl supervises the controller services) or passed
  over a local socket (`SCM_RIGHTS`) for intra-mode handovers such as the GRBL
  `$H` cloud-homing session. The writer writes the pulse ring **directly**
  through its fd: the real-time feed path is never proxied through forgectrl.
- **Exactly one writer:** forgectrl hands out at most one motion dup at a time
  and hands it out only with the kernel idle. The laser latch is locked across
  every handover.
- **Dead-man, re-plumbed:** with forgectrl holding a reference, a writer crash
  no longer closes the file, so the kernel dead-man does not fire on it.
  forgectrl is the dead-man for its writers: it supervises them (child exit /
  socket close) and on writer death immediately writes `cnc/stop` and
  `cnc/laser_latch=1`, before any re-handover. Below that remain the kernel's
  own backstops (ring drains → end-of-data forces all output lines low;
  underrun faults) and the hardware AND-gate, which is the actual safety
  boundary. forgectrl itself dying closes the file and trips the kernel
  dead-man exactly as today — a conservative failure.
- **Rail policy is forgectrl's alone:** `cnc/enable`/`cnc/disable` are written
  only by forgectrl (and diagnostics under its takeover rules). Controllers and
  the cloud client's shutdown path must not disable the rail when handing the
  device back. Every deliberate re-enable observes the settle period
  (`rail_settle_s`); forgectrl may drop the rail after a configurable idle
  period, always restoring it through a settled power-up before the next run.

---

## Hardware ownership [contract]

Single-writer rule: each attr group has exactly one writing process per mode.
Readers are unrestricted.

| Hardware | GRBL mode | Cloud mode | Diagnostics | Target (engine landed) |
|---|---|---|---|---|
| `thermal/*` fans/pump/TEC/heater, `head/air_assist_pwm`, `head/purge_air` | GRBL controller | cloud client | forgectrl (controller stopped) | **forgectrl only** (+ the stale-verdict fallback) |
| `cnc/*` motion, `/dev/glowforge` ring | GRBL controller | cloud client | — | active controller, via an fd brokered by forgectrl (see Pulse-device ownership) |
| `cnc/enable` / `cnc/disable` (40 V rail) | GRBL controller | cloud client | forgectrl | **forgectrl only** (rail policy + settle) |
| `cnc/laser_latch` | GRBL controller | cloud client | — | active controller (locked by forgectrl across handovers and on writer death) |
| Button LEDs (`/sys/class/leds/button_led_*`) | GRBL controller (arm flow) | cloud client | — | active controller |
| Head/lid illumination (camera lamps) | forgectrl (`lamp` on snapshot) | forgectrl | forgectrl | forgectrl |
| Cameras (V4L2, MIPI mux) | forgectrl | forgectrl | forgectrl | forgectrl |
| `/data/forgefirm.conf` settings | read (re-read per `$H` / flood start) | read | read | forgectrl writes, all read |
| `/run/grblhal.homed` anchor | GRBL controller writes | — | — | unchanged |
| `/run/forgefirm/cooling.state` | — | — | — | forgectrl writes, controllers read |

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

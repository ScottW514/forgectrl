# forgectrl

System control daemon for [ForgeFIRM](https://github.com/ScottW514/forgefirm)-powered
Glowforge lasers.

forgectrl runs on the factory i.MX6 control board as the machine-services
daemon (HTTP on port 8080). Motion is executed by exactly one of two
controllers — [grblHAL-glowforge](https://github.com/ScottW514/grblHAL-glowforge)
(GRBL mode) or the gfcloud web-service client (factory cloud mode) — and
forgectrl owns everything around them:

- **Controller-mode supervision** — the selected controller runs as a
  direct child; `POST /mode` switches modes live (idle-gated), a crashed
  controller is respawned after the machine is safed, and forgectrl
  itself runs under a respawn wrapper that retakes supervision once the
  machine is idle.
- **The pulse-device broker** — forgectrl holds `/dev/glowforge`
  (exclusive-open) for its lifetime and controllers inherit the fd, so
  mode switches, homing handovers, and respawns never close the device
  or cycle the 40 V motor rail; the supervisor is the writers' dead-man.
- **The motion-liveness gate** — the stepper drivers can come out of a
  rail power-up unserviceable while every counter runs normally, so
  before each session's first controller spawn the supervisor commands a
  small probe move and verifies it *physically happened* via the head
  accelerometer, with a rail-off recovery ladder and an explicit
  `motion-fault` state.
- **The cooling engine** — the single owner of fans, pump, TEC, and the
  flow-check heater for both modes: coolant-flow verification, over-temp
  hold/resume policy, and per-job fan profiles, fed by controller
  job-state reports (`POST /cool/state`) and publishing a verdict file
  the controllers enforce in-process. Every gate is a plain setting
  with a wide range whose far end turns it off by value; the panel
  warns outside the recommended band and while any gate is off.
- The **web control panel**, **camera service**, **telemetry**,
  **diagnostics**, persisted **machine settings**, the **logging**
  tree (levels, viewer, sanitized export), and the A/B **update
  system**.

The shared contract — switch maps, sensor conversions, hardware
ownership, the cooling channels, mode supervision, pulse-device
ownership, and logging — is [docs/SERVICES.md](docs/SERVICES.md).

## The control panel (`GET /`)

A self-contained single page (no external assets) carrying the
OpenGlow visual identity, tabbed:

- **Status** — the live controller-mode selector (switches through the
  supervisor; the setting persists for boot), live operational status
  (motion state and position, coolant temperatures and fan tachometers,
  safety-switch states, system summary), plus a scaled lid-camera
  snapshot that switches to the live MJPEG stream on demand.
- **Machine** — shared settings: display units, homing method and the
  post-homing position calibration, cooling tunables.
- **GF Cloud** — Glowforge web-service overrides: machine identity
  (serial / password; blank = the factory fuse identity) and the
  homing-session timeout.
- **GRBL** — controller connection info and the GRBL-mode tunables the
  controller reads from the shared settings: the laser arm window
  (button wait, disarm grace) and the motor-rail settle time.
- **Diagnostics** — tools that take the hardware over (the active
  controller is suspended through the supervisor for the duration):
  cooling system verification and calibration.
- **Logs** — per-logger disk and remote log levels (applied at the next
  reboot), the remote syslog target, a live log viewer, and the log
  export (sanitized by default) for issue reports.
- **System** — firmware slots (A/B boot selection), ForgeFIRM updates,
  image install/restore, the WiFi regulatory region (power save is
  kept off), reboot.

The design intent: every machine tunable — shared, cloud-override, and
GRBL-mode — gets a home in one of these tabs as it appears.

## Machine settings

Settings persist in `/data/forgefirm.conf`, shared with the
grblHAL-glowforge controller (re-read on every `$H`) and the gfhome
homing runner (read at session start), so changes apply without
restarts.

| Endpoint | Purpose |
|---|---|
| `GET /status` | Machine operational status as JSON (state, position when homed, fans, coolant, switches, `gates_off`) |
| `GET /settings` | Current settings as JSON (plus the system hostname, firmware version, and the `gates` table: range, recommended band, off end and state per gate setting) |
| `POST /settings?key=value&...` | Set any subset of known keys |
| `GET /mode` | Supervisor state: mode, controller (`running`/`stopped`/`standby`/`motion-fault`), pid, motion verdict |
| `POST /mode?controller=grbl\|cloud` | Live idle-gated mode switch; also the retry lever after a motion fault |
| `POST /cool/state` | Controller job-state report (mode, armed, per-job run fan duties), level-triggered ~1 Hz |
| `GET /cool/status` | Cooling-engine state: phase, verdict, temps, report age, `gates_off` |

Position comes from the kernel step counters anchored at the last
completed homing (`/run/grblhal.homed`, written by the controller) —
the Grbl TCP socket is never queried, since a connection there would
displace the sender's session.

An empty value clears a key back to its built-in default (send clears as
query parameters). Writes are refused (409) unless the machine is idle —
the controller and the homing runner both read this file mid-run. Known
keys:

| Key | Meaning |
|---|---|
| `controller_mode` | `grbl` or `cloud` — the boot-time mode; `POST /mode` switches live and persists it |
| `homing_mode` | `$H` behavior: `gfcloud`, `switches`, or `none` |
| `gfcloud_home_x/y/z` | Machine coordinates after a completed homing (mm) |
| `gfcloud_home_timeout_s` | Web-service homing session budget (30–3600 s) |
| `gf_serial` | Cloud sign-in serial override (digits) |
| `gf_password` | Cloud sign-in password override (64 hex; write-only — `GET` reports `gf_password_set`) |
| `ui_units` | Panel display units: `metric` or `imperial` (values are stored and exchanged in metric) |
| `cool_*` | Coolant-loop protection tunables (flow-check bands, temperature ceiling/resume, cooldown) — see the Machine tab hints |
| `wifi_country` | WiFi regulatory region, ISO 3166-1 alpha-2; unset = automatic (the AP's 802.11d country, else world). Applied via `iw reg reload`/`iw reg set` at startup and on change; power save is pinned off in the same pass |
| `log_<logger>_disk`, `log_<logger>_remote` | Log level per logger (`forgectrl`, `grblhal`, `gfcloud`, `gfhome`, `kernel`, `system`) and destination: `off`, `error`, `warning`, `notice`, `info` (disk default), `debug`; remote defaults to `off`. Applied at the next reboot |
| `syslog_server`, `syslog_port`, `syslog_proto` | Remote syslog target (host or address; 514; `udp` or `tcp`). Nothing is forwarded until a server is set. Applied at the next reboot |

## Logging

Every ForgeFIRM process emits through the system syslog socket
(forgectrl and the grblHAL driver through the shared non-blocking
`fflog` emitter in `src/fflog.[ch]`, the Python apps through
`SysLogHandler`); rsyslog is the only file writer and files each program
under its own directory, `/data/log/forgefirm/<logger>/`, size-capped
and rotated. A process emits at the more verbose of its two configured
levels and rsyslog filters per destination; the kernel's levels only
filter what printk emits. The stray stdout/stderr of a controller (an
interpreter traceback, a library message) reaches syslog through a
`logger` relay under the controller's own name; the daemon's own stray
output takes the same route through its init script.

`forgectrl --render-syslog` writes the rsyslog rules
(`/data/forgefirm/rsyslog-forgefirm.conf`) and the log directories from
the settings; the boot sequence runs it before rsyslog starts, which is
why level changes apply at reboot. The panel shows configured against
effective levels and offers the reboot.

| Endpoint | Purpose |
|---|---|
| `GET /logs` | Loggers with configured and effective levels and on-disk sizes, the remote target, `pending_reboot` |
| `GET /logs/tail?name=&lines=&from=` | The last `lines` of a logger's live file, or everything since byte offset `from` (incremental follow) |
| `POST /logs/export?sanitize=1\|0` | Streams a `tar.gz` of every logger's files plus a system snapshot (version, dmesg, uptime, memory, disk, processes, effective levels, settings with secrets masked). Sanitized by default: known identifiers (serial, hostname, cloud credentials, panel token, WiFi network) and pattern classes (network addresses, e-mail addresses, bearer/basic credentials, JWTs, key=value secrets, long hex/base64 blobs) become placeholders that stay stable within the bundle (`src/sanitize.c`; `tests/sanitize_test.c` in CI) |

All three require the panel token. `tests/fflog_e2e.sh` proves the whole
path on a host against a private rsyslogd (emitter, relay, format,
per-logger filtering).

## Camera service

Both OV5648 cameras (lid and head) as MJPEG over the mainline imx-media
pipeline:

| Endpoint | Purpose |
|---|---|
| `GET /` | Index page: stream toggle, head peek, snapshots, status |
| `GET /cam/stream?cam=lid\|head` | Multipart MJPEG, 1296×972 |
| `GET /cam/snapshot?cam=lid\|head&res=full\|half&q=1..100` | Single JPEG, default full 2592×1944 |
| `GET /cam/status` | JSON: running/cam/clients/frames/fps/fps_cap/encoder/buffers |
| `GET /?action=stream` / `/?action=snapshot` | mjpg-streamer-compatible aliases (lid) |

The stream demosaics each 2×2 BGGR quad to one pixel (NEON kernel) straight
into planar YUV420 and encodes on the i.MX6 CODA960 VPU JPEG encoder —
15 fps at 1296×972 on a Glowforge (sensor-limited). Snapshots use a bilinear
demosaic and libjpeg, which is also the automatic fallback encoder.

Capture buffers are requested non-coherent (CPU-cached) so the demosaic can
read frames in place; on kernels whose capture queue lacks cache-hint
support the daemon falls back to uncached buffers with a bounce copy
(`/cam/status` reports which as `"buffers"`).

The cameras share the hardware video-mux; the newest request wins it:

- A stream request for the other camera preempts current stream clients
  (their streams end cleanly; viewers freeze on the last frame) and switches.
- A snapshot of the other camera does not switch: the engine borrows the mux
  for one frame and the stream freezes for a second or two.

The capture pipeline is held open while clients are active and fully released
after 10 s idle, so one-shot V4L2 users can still grab. The per-camera
illumination LED is raised during capture and restored on idle.

## Environment

| Variable | Default | Purpose |
|---|---|---|
| `FORGECTRL_PORT` | 8080 | HTTP port |
| `FORGECTRL_STREAM_Q` | 75 | Stream JPEG quality (1–100) |
| `FORGECTRL_STREAM_FPS` | unset | Stream frame-rate ceiling (frames/s); unset or 0 = sensor max |
| `FORGECTRL_LAMP` | 132 | Illumination level during capture (0–1023) |
| `FORGECTRL_NO_VPU` | unset | Force the libjpeg software encoder |
| `FORGECTRL_NO_NEON` | unset | Force the scalar demosaic |
| `FORGECTRL_NO_CACHED_BUFS` | unset | Force uncached capture buffers + bounce copy |
| `FORGECTRL_NEON_CHECK` | unset | One-shot NEON/scalar equivalence check (logged) |
| `FFLOG_LEVEL` | from settings | Override the emit level (`off`..`debug`) |
| `FFLOG_STDERR` | unset | Echo log lines to stderr even when it is not a terminal (harnesses) |
| `FFLOG_CONF`, `FFLOG_SOCK` | `/data/forgefirm.conf`, `/dev/log` | Settings file and syslog socket (host tests) |

## Building

CMake; links against ulfius, libjpeg and zlib. Runtime needs Linux with imx-media,
the coda VPU driver, and v4l-utils (`media-ctl`/`v4l2-ctl`) on the target.
The ForgeFIRM Yocto layer (`meta-forgefirm` in the forgefirm repo) carries
the recipe, which also installs the sysvinit script from `init/`.

## Developing the panel

The panel is a plain static page under `src/ui/` - `index.html`,
`panel.css`, `panel.js` (ES5, no external assets). The build bundles the
three into one self-contained page and embeds it in the daemon
(`src/ui/embed.cmake`, run by CMake; the bundled page also lands in
`build/ui/index.html`), so what ships is still a single response with the
token substituted at serve time. `tools/devserver.py` (Python 3, standard
library only) serves the files as they are, with live reload: the browser
sees real file names and line numbers, and the open tab reloads whenever
anything under `src/ui/` is saved (`--bundle` serves the page inlined the
way the daemon does). API calls from the page go to one of two backends:

- **A real machine.** `GF_HOST` (IP literal, `:port` if not 8080) and
  `GF_TOKEN` (the panel token, `/data/forgefirm/panel.token` on the
  machine) in the environment or in a git-ignored `.env` at the repo root
  (`.env.example` is the template; the file is re-read when it changes).
  The token is embedded in the served page exactly as the daemon does it,
  requests are proxied with the machine's address-literal `Host`, and the
  MJPEG stream passes through - so the panel shows live data and its
  actions reach the hardware.
- **The built-in mock** (`--mock`, or automatically without `GF_HOST`):
  in-memory settings, status, diagnostics, slots, logs and a placeholder
  camera, with the daemon's token check mirrored on state-changing calls.

```
cp .env.example .env            # then fill in GF_HOST / GF_TOKEN
python3 tools/devserver.py      # http://127.0.0.1:8081
python3 tools/devserver.py --mock
python3 tools/devserver.py --dump > panel.html   # the bundled page
```

`.devcontainer/` packages this for VS Code (Dev Containers, Docker or
Podman): Ubuntu 24.04 with the host build dependencies, so `cmake -B build
&& cmake --build build` and the CI unit tests also run inside; the "panel:
dev server" task starts the dev server when the folder opens and port
8081 is forwarded. Interactive shells in the container export `.env`, so
the ForgeFIRM bench tools see `GF_HOST` / `GF_TOKEN` too.

## License

MIT — see [LICENSE](LICENSE).

# forgectrl

System control daemon for [ForgeFIRM](https://github.com/ScottW514/forgefirm)-powered
Glowforge lasers.

forgectrl runs on the factory i.MX6 control board and exposes the machine's
non-motion functions over HTTP (port 8080). Motion is handled by the separate
[grblHAL-glowforge](https://github.com/ScottW514/grblHAL-glowforge) controller;
forgectrl is the control surface around it: a web control panel, the camera
service, and the persisted machine settings shared with the controller and
the gfhome homing runner. Realtime hardware status/control and
controller-mode selection (GRBL / Glowforge cloud) are planned here.

## The control panel (`GET /`)

A self-contained single page (no external assets), tabbed:

- **Status** — machine summary plus a scaled lid-camera snapshot that
  switches to the live MJPEG stream on demand.
- **Machine** — shared settings: homing method and the post-homing
  position calibration.
- **GF Cloud** — Glowforge web-service overrides: machine identity
  (serial / password / hostname; blank = the factory fuse identity) and
  the homing-session timeout.
- **GRBL** — controller connection info; GRBL-mode tunables land here as
  the controller exposes them.

The design intent: every machine tunable — shared, cloud-override, and
GRBL-mode — gets a home in one of these tabs as it appears.

## Machine settings

Settings persist in `/data/forgefirm.conf`, shared with the
grblHAL-glowforge controller (re-read on every `$H`) and the gfhome
homing runner (read at session start), so changes apply without
restarts.

| Endpoint | Purpose |
|---|---|
| `GET /settings` | Current settings as JSON (plus the system hostname) |
| `POST /settings?key=value&...` | Set any subset of known keys |

An empty value clears a key back to its built-in default (send clears as
query parameters). Known keys:

| Key | Meaning |
|---|---|
| `homing_mode` | `$H` behavior: `gfcloud`, `switches`, or `none` |
| `gfcloud_home_x/y/z` | Machine coordinates after a completed homing (mm) |
| `gfcloud_home_timeout_s` | Web-service homing session budget (30–3600 s) |
| `gf_serial` | Cloud sign-in serial override (digits) |
| `gf_password` | Cloud sign-in password override (64 hex; write-only — `GET` reports `gf_password_set`) |
| `gf_hostname` | Reported hostname override |

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

## Building

CMake; links against ulfius and libjpeg. Runtime needs Linux with imx-media,
the coda VPU driver, and v4l-utils (`media-ctl`/`v4l2-ctl`) on the target.
The ForgeFIRM Yocto layer (`meta-forgefirm` in the forgefirm repo) carries
the recipe, which also installs the sysvinit script from `init/`.

## License

MIT — see [LICENSE](LICENSE).

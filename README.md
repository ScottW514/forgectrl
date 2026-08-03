# forgectrl

System control daemon for [ForgeFIRM](https://github.com/ScottW514/forgefirm)-powered
Glowforge lasers.

forgectrl runs on the factory i.MX6 control board and exposes the machine's
non-motion functions over HTTP (port 8080). Motion is handled by the separate
[grblHAL-glowforge](https://github.com/ScottW514/grblHAL-glowforge) controller;
forgectrl is the control surface around it. Camera service is implemented;
realtime hardware status/settings, hardware control, and controller-mode
selection (GRBL / Glowforge cloud) are planned here.

## Camera service

Both OV5648 cameras (lid and head) as MJPEG over the mainline imx-media
pipeline:

| Endpoint | Purpose |
|---|---|
| `GET /` | Index page: stream toggle, head peek, snapshots, status |
| `GET /cam/stream?cam=lid\|head` | Multipart MJPEG, 1296×972 |
| `GET /cam/snapshot?cam=lid\|head&res=full\|half&q=1..100` | Single JPEG, default full 2592×1944 |
| `GET /cam/status` | JSON: running/cam/clients/frames/fps/encoder |
| `GET /?action=stream` / `/?action=snapshot` | mjpg-streamer-compatible aliases (lid) |

The stream demosaics each 2×2 BGGR quad to one pixel (NEON kernel) straight
into planar YUV420 and encodes on the i.MX6 CODA960 VPU JPEG encoder —
15 fps at 1296×972 on a Glowforge (sensor-limited). Snapshots use a bilinear
demosaic and libjpeg, which is also the automatic fallback encoder.

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
| `FORGECTRL_LAMP` | 132 | Illumination level during capture (0–1023) |
| `FORGECTRL_NO_VPU` | unset | Force the libjpeg software encoder |
| `FORGECTRL_NO_NEON` | unset | Force the scalar demosaic |
| `FORGECTRL_NEON_CHECK` | unset | One-shot NEON/scalar equivalence check (logged) |

## Building

CMake; links against ulfius and libjpeg. Runtime needs Linux with imx-media,
the coda VPU driver, and v4l-utils (`media-ctl`/`v4l2-ctl`) on the target.
The ForgeFIRM Yocto layer (`meta-forgefirm` in the forgefirm repo) carries
the recipe, which also installs the sysvinit script from `init/`.

## License

MIT — see [LICENSE](LICENSE).

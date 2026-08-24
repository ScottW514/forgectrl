/*
 * main.c - forgectrl: ForgeFIRM system control daemon
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * HTTP service (ulfius) exposing the Glowforge cameras as MJPEG:
 *
 *   GET /                        the machine control panel (src/ui/)
 *   GET /?action=stream          mjpg-streamer-compatible stream (lid)
 *   GET /?action=snapshot        mjpg-streamer-compatible snapshot (lid)
 *   GET /cam/stream?cam=lid|head            multipart MJPEG, half sensor res
 *   GET /cam/h264?cam=lid|head              fragmented MP4 H.264 live stream
 *                                (MSE-consumable; far fewer bytes than MJPEG)
 *   GET /cam/snapshot?cam=&res=full|half&q=&lamp=  single JPEG (full res;
 *                                lamp overrides the scene lamp for the shot)
 *   GET /cam/status                         JSON engine status, including
 *                                the bound sensor, its frame geometry, and
 *                                whether the lid currently permits capture
 *
 * The cameras only capture with the lid closed (a privacy rule, enforced
 * in cam.c): stream and snapshot answer 409 while it is open.
 *   GET /status                             JSON machine operational status
 *   GET /settings                           JSON machine settings
 *   POST /settings?key=value                update machine settings
 *   GET /fuse-identity                      burned-in identity (on demand)
 *   GET /logs                               loggers, levels, sizes
 *   GET /logs/tail?name=&lines=&from=       a logger's live file (follow)
 *   POST /logs/export?sanitize=1|0          tar.gz bundle of all logs
 *
 * Invocation: forgectrl [--render-syslog]. --render-syslog writes the
 * rsyslog rules and log directories from the settings and exits; the
 * boot sequence runs it before rsyslog starts (see logs.c).
 *
 * The two cameras share the hardware mux; the newest request wins it. A
 * STREAM request for the other camera preempts the current stream
 * clients (their streams end cleanly - viewers freeze on the last frame)
 * and switches. A SNAPSHOT of the other camera does not switch: the
 * engine borrows the mux for one frame and the stream freezes briefly.
 * Environment: FORGECTRL_PORT (8080), FORGECTRL_STREAM_Q (75),
 * FORGECTRL_LAMP (132), FORGECTRL_STREAM_FPS (0 = sensor max; 1 or more
 * is also realized as CSI hardware frame skip), FORGECTRL_H264_KBPS
 * (1500), FORGECTRL_H264_GOP (30), and the fallback switches
 * FORGECTRL_NO_VPU, FORGECTRL_NO_H264, FORGECTRL_NO_GPU,
 * FORGECTRL_NO_HW_SKIP, FORGECTRL_NO_CACHED_BUFS, FORGECTRL_NO_NEON.
 *
 * ulfius runs libmicrohttpd in thread-per-connection mode, so each stream
 * callback may block waiting for the next frame.
 */
#define _GNU_SOURCE
#include "auth.h"
#include "cam.h"
#include "mp4mux.h"
#include "cool.h"
#include "diag.h"
#include "fflog.h"
#include "gates.h"
#include "logs.h"
#include "settings.h"
#include "status.h"
#include "super.h"
#include "ui.h"
#include "update.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <ulfius.h>
#include <unistd.h>
#include <zlib.h>

#define DEFAULT_PORT 8080
#define BOUNDARY     "forgectrl-frame"
#define SNAP_Q_DEF   75

static volatile sig_atomic_t quit = 0;

static void on_signal(int sig)
{
    (void)sig;
    quit = 1;
}

/* ------------------------------------------------------------- helpers */

static cam_id_t parse_cam(const struct _u_request *req, int *ok)
{
    const char *v = u_map_get(req->map_url, "cam");
    *ok = 1;
    if (!v || !strcmp(v, "lid"))
        return CAM_LID;
    if (!strcmp(v, "head"))
        return CAM_HEAD;
    *ok = 0;
    return CAM_LID;
}

static int reply_error(struct _u_response *res, unsigned status,
                       const char *msg)
{
    ulfius_set_string_body_response(res, status, msg);
    ulfius_add_header_to_response(res, "Content-Type", "text/plain");
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------ streaming */

struct stream_ctx {
    cam_client_t *cl;
    uint8_t      *chunk;    /* current multipart chunk being drained */
    size_t        chunk_cap;
    size_t        chunk_len;
    size_t        off;
};

static ssize_t stream_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    struct stream_ctx *sc = cls;

    if (sc->off >= sc->chunk_len) {
        const uint8_t *jpg;
        long len = cam_client_next(sc->cl, &jpg);
        if (len < 0)
            return U_STREAM_END;

        char head[128];
        int headlen = snprintf(head, sizeof(head),
                               "--" BOUNDARY "\r\n"
                               "Content-Type: image/jpeg\r\n"
                               "Content-Length: %ld\r\n\r\n", len);
        size_t need = (size_t)headlen + (size_t)len + 2;
        if (sc->chunk_cap < need) {
            uint8_t *nb = realloc(sc->chunk, need);
            if (!nb)
                return U_STREAM_ERROR;
            sc->chunk = nb;
            sc->chunk_cap = need;
        }
        memcpy(sc->chunk, head, (size_t)headlen);
        memcpy(sc->chunk + headlen, jpg, (size_t)len);
        memcpy(sc->chunk + headlen + len, "\r\n", 2);
        sc->chunk_len = need;
        sc->off = 0;
    }

    size_t n = sc->chunk_len - sc->off;
    if (n > max)
        n = max;
    memcpy(buf, sc->chunk + sc->off, n);
    sc->off += n;
    return (ssize_t)n;
}

static void stream_free_cb(void *cls)
{
    struct stream_ctx *sc = cls;
    cam_client_close(sc->cl);
    free(sc->chunk);
    free(sc);
}

/* ------------------------------------------------- H.264 (fMP4) stream */

static unsigned cam_err_status(const char *err);

struct h264_ctx {
    cam_h264_client_t *cl;
    mp4mux_t          *mux;
    uint32_t           frag_seq;
    uint64_t           pts_base;    /* first frame's clock: fragments are
                                     * zero-based so any player starts at
                                     * the top of its timeline */
    uint64_t           prev_pts;
    uint8_t           *chunk;       /* current fMP4 piece being drained */
    size_t             chunk_len;
    size_t             off;
    uint8_t           *pending;     /* init segment queued before frame 1 */
    size_t             pending_len;
};

/* Pull one access unit and wrap it as a moof+mdat chunk. */
static int h264_next_chunk(struct h264_ctx *hc)
{
    const uint8_t *au;
    uint64_t pts;
    int key;
    long len = cam_h264_next(hc->cl, &au, &pts, &key);
    if (len < 0)
        return -1;
    uint32_t dur = 90000 / 15;
    if (hc->frag_seq > 0 && pts > hc->prev_pts &&
        pts - hc->prev_pts < 90000)
        dur = (uint32_t)(pts - hc->prev_pts);
    hc->prev_pts = pts;
    free(hc->chunk);
    hc->chunk = mp4mux_fragment(hc->mux, ++hc->frag_seq,
                                pts - hc->pts_base, dur, key,
                                au, (size_t)len, &hc->chunk_len);
    hc->off = 0;
    return hc->chunk ? 0 : -1;
}

static ssize_t h264_cb(void *cls, uint64_t pos, char *buf, size_t max)
{
    (void)pos;
    struct h264_ctx *hc = cls;

    if (hc->off >= hc->chunk_len) {
        if (hc->pending) {
            free(hc->chunk);
            hc->chunk = hc->pending;
            hc->chunk_len = hc->pending_len;
            hc->pending = NULL;
            hc->off = 0;
        } else if (h264_next_chunk(hc)) {
            return U_STREAM_END;
        }
    }
    size_t n = hc->chunk_len - hc->off;
    if (n > max)
        n = max;
    memcpy(buf, hc->chunk + hc->off, n);
    hc->off += n;
    return (ssize_t)n;
}

static void h264_free_cb(void *cls)
{
    struct h264_ctx *hc = cls;
    cam_h264_client_close(hc->cl);
    mp4mux_free(hc->mux);
    free(hc->chunk);
    free(hc->pending);
    free(hc);
}

static int do_h264(cam_id_t cam, struct _u_response *res)
{
    char err[256];
    cam_h264_client_t *cl = cam_h264_client_open(cam, err, sizeof(err));
    if (!cl)
        return reply_error(res, cam_err_status(err), err);

    struct h264_ctx *hc = calloc(1, sizeof(*hc));
    if (!hc) {
        cam_h264_client_close(cl);
        return reply_error(res, 500, "out of memory");
    }
    hc->cl = cl;

    /* Block for the first access unit here, so the codec string (from
     * the SPS) can travel in a response header and the init segment
     * precedes frame one. A machine whose H.264 encoder is missing or
     * refused answers 503 instead of an empty stream. */
    const uint8_t *au;
    uint64_t pts;
    int key;
    long len = cam_h264_next(cl, &au, &pts, &key);
    uint8_t params[512];
    size_t plen = len < 0 ? 0 : cam_h264_params(params, sizeof(params));
    if (len < 0 || plen == 0) {
        h264_free_cb(hc);
        return reply_error(res, 503, "H.264 stream unavailable "
                           "(the MJPEG stream still works)");
    }
    struct cam_status st;
    cam_get_status(&st);
    hc->mux = mp4mux_new(st.stream_w, st.stream_h);
    if (!hc->mux || !mp4mux_feed_params(hc->mux, params, plen)) {
        h264_free_cb(hc);
        return reply_error(res, 500, "H.264 parameter sets unusable");
    }
    size_t init_len = 0, frag_len = 0;
    uint8_t *init = mp4mux_init_segment(hc->mux, &init_len);
    hc->pts_base = pts;
    hc->prev_pts = pts;
    uint8_t *frag = mp4mux_fragment(hc->mux, ++hc->frag_seq, 0,
                                    90000 / 15, key, au, (size_t)len,
                                    &frag_len);
    if (!init || !frag) {
        free(init);
        free(frag);
        h264_free_cb(hc);
        return reply_error(res, 500, "out of memory");
    }
    hc->chunk = init;
    hc->chunk_len = init_len;
    hc->off = 0;
    hc->pending = frag;
    hc->pending_len = frag_len;

    ulfius_add_header_to_response(res, "Content-Type", "video/mp4");
    ulfius_add_header_to_response(res, "X-H264-Codec",
                                  mp4mux_codec(hc->mux));
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_stream_response(res, 200, h264_cb, h264_free_cb,
                               U_STREAM_SIZE_UNKNOWN, 64 * 1024, hc);
    return U_CALLBACK_CONTINUE;
}

/* Camera failures that reflect machine state rather than a fault answer
 * 409 (the request conflicts with how the machine is right now, and the
 * fix is to change that): the mux is held by another viewer, or the lid
 * is open and the privacy gate refuses to capture. Everything else is a
 * 503 - the camera could not be brought up. */
static unsigned cam_err_status(const char *err)
{
    return (strstr(err, "busy") || !strcmp(err, CAM_ERR_LID)) ? 409 : 503;
}

static int do_stream(cam_id_t cam, struct _u_response *res)
{
    char err[256];
    cam_client_t *cl = cam_client_open(cam, err, sizeof(err));
    if (!cl)
        return reply_error(res, cam_err_status(err), err);

    struct stream_ctx *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        cam_client_close(cl);
        return reply_error(res, 500, "out of memory");
    }
    sc->cl = cl;

    ulfius_add_header_to_response(res, "Content-Type",
        "multipart/x-mixed-replace; boundary=" BOUNDARY);
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_stream_response(res, 200, stream_cb, stream_free_cb,
                               U_STREAM_SIZE_UNKNOWN, 64 * 1024, sc);
    return U_CALLBACK_CONTINUE;
}

static int do_snapshot(cam_id_t cam, int full, int quality, int lamp,
                       struct _u_response *res)
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    char err[256];
    if (cam_snapshot(cam, full, quality, lamp, &jpg, &len, err, sizeof(err)))
        return reply_error(res, cam_err_status(err), err);
    ulfius_set_binary_body_response(res, 200, (const char *)jpg, len);
    ulfius_add_header_to_response(res, "Content-Type", "image/jpeg");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    free(jpg);
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------ callbacks */

static int cb_stream(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    int ok;
    cam_id_t cam = parse_cam(req, &ok);
    if (!ok)
        return reply_error(res, 400, "cam must be 'lid' or 'head'");
    return do_stream(cam, res);
}

static int cb_h264(const struct _u_request *req, struct _u_response *res,
                   void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    int ok;
    cam_id_t cam = parse_cam(req, &ok);
    if (!ok)
        return reply_error(res, 400, "cam must be 'lid' or 'head'");
    return do_h264(cam, res);
}

static int cb_snapshot(const struct _u_request *req, struct _u_response *res,
                       void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    int ok;
    cam_id_t cam = parse_cam(req, &ok);
    if (!ok)
        return reply_error(res, 400, "cam must be 'lid' or 'head'");

    int full = 1;
    const char *v = u_map_get(req->map_url, "res");
    if (v) {
        if (!strcmp(v, "half"))
            full = 0;
        else if (strcmp(v, "full"))
            return reply_error(res, 400, "res must be 'full' or 'half'");
    }
    int quality = SNAP_Q_DEF;
    if ((v = u_map_get(req->map_url, "q")) != NULL) {
        quality = atoi(v);
        if (quality < 1 || quality > 100)
            return reply_error(res, 400, "q must be 1..100");
    }
    int lamp = -1;
    if ((v = u_map_get(req->map_url, "lamp")) != NULL) {
        lamp = atoi(v);
        if (lamp < 0 || lamp > 1023)
            return reply_error(res, 400, "lamp must be 0..1023");
    }
    return do_snapshot(cam, full, quality, lamp, res);
}

static int cb_status(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    struct cam_status st;
    cam_get_status(&st);
    char body[768];
    snprintf(body, sizeof(body),
             "{\"running\":%s,\"cam\":\"%s\",\"clients\":%d,"
             "\"frames\":%llu,\"fps\":%.1f,\"fps_cap\":%.1f,"
             "\"hw_fps_skip\":%s,"
             "\"encoder\":\"%s\",\"convert\":\"%s\",\"buffers\":\"%s\","
             "\"sensor\":\"%s\","
             "\"stream\":{\"width\":%d,\"height\":%d},"
             "\"snapshot\":{\"width\":%d,\"height\":%d},"
             "\"h264\":{\"active\":%s,\"clients\":%d},"
             "\"health\":{\"captured\":%llu,\"corrupt\":%llu,"
             "\"restarts\":%u},"
             "\"capture_allowed\":%s,\"stopped_by_lid\":%s}",
             st.running ? "true" : "false", cam_name(st.cam), st.clients,
             (unsigned long long)st.seq, st.fps, st.fps_cap,
             st.hw_skip ? "true" : "false",
             st.vpu ? "vpu" : "software",
             st.gpu ? "gpu" : "cpu",
             st.cached ? "cached" : "uncached", st.sensor,
             st.stream_w, st.stream_h, st.snap_w, st.snap_h,
             st.h264_up ? "true" : "false", st.h264_clients,
             (unsigned long long)st.frames, (unsigned long long)st.corrupt,
             st.recoveries,
             st.lid_closed ? "true" : "false",
             st.lid_stopped ? "true" : "false");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------- settings */

/* Machine settings shared with the grblHAL-glowforge controller and the
 * gfhome homing runner through /data/forgefirm.conf. The controller
 * re-reads the file on every $H and the runner at every session start,
 * so changes apply without restarts. Every key is validated here; an
 * empty value removes the key (back to the built-in default) and must
 * arrive as a query parameter - zero-length form-body values never
 * reach the body map. Secret keys are write-only: GET reports
 * "<key>_set" instead of the value. */

static int valid_homing_mode(const char *v)
{
    return !strcmp(v, "none") || !strcmp(v, "gfcloud") ||
           !strcmp(v, "switches");
}

static int valid_controller_mode(const char *v)
{
    /* grbl = grblHAL over TCP:23; cloud = the Glowforge web-service stack
     * (gfcloud daemon). The two are mutually exclusive; the boot-time init
     * scripts dispatch on this key, so a change applies on the next
     * controller restart. */
    return !strcmp(v, "grbl") || !strcmp(v, "cloud");
}

/* Numeric values are short by construction; a long-but-valid string
 * (e.g. "000...0033.0") is rejected here so the settings-report buffer
 * math can never be driven to overflow by an accepted value. */
#define VALUE_MAX_LEN 16

static int valid_mm(const char *v)
{
    char *end;
    if (strlen(v) > VALUE_MAX_LEN)
        return 0;
    double f = strtod(v, &end);
    return end != v && *end == '\0' && f >= -1000.0 && f <= 1000.0;
}

static int valid_timeout(const char *v)
{
    char *end;
    if (strlen(v) > VALUE_MAX_LEN)
        return 0;
    long t = strtol(v, &end, 10);
    return end != v && *end == '\0' && t >= 30 && t <= 3600;
}

static int valid_serial(const char *v)
{
    size_t n = strlen(v);
    if (n < 1 || n > 12)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (v[i] < '0' || v[i] > '9')
            return 0;
    return 1;
}

static int valid_password(const char *v)
{
    if (strlen(v) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (!isxdigit((unsigned char)v[i]))
            return 0;
    return 1;
}

static int valid_units(const char *v)
{
    return !strcmp(v, "metric") || !strcmp(v, "imperial");
}

/* WiFi regulatory region: ISO 3166-1 alpha-2, or "00" for the world
 * domain */
static int valid_country(const char *v)
{
    if (!strcmp(v, "00"))
        return 1;
    return strlen(v) == 2 &&
           v[0] >= 'A' && v[0] <= 'Z' && v[1] >= 'A' && v[1] <= 'Z';
}

/* Cooling tunables (consumed by the controller per flood start). The
 * ranges are wide on purpose - these exist for per-machine calibration
 * (pump wear, replacement coolant) - but still bounded to values the
 * hardware can mean something by. */
static int valid_range(const char *v, double lo, double hi)
{
    char *end;
    if (strlen(v) > VALUE_MAX_LEN)
        return 0;
    double f = strtod(v, &end);
    return end != v && *end == '\0' && f >= lo && f <= hi;
}

/* The gate settings (the coolant ceiling and its resume gate, the flow
 * window and its fault rise) validate against the one table in gates.c:
 * a wide legal range whose far end turns the gate off by value, and a
 * recommended band the panel warns outside of. The rest of the cooling
 * keys are bounded to what the engine can mean by them. */
static int valid_gate(const char *key, const char *v)
{
    return gate_parse(gate_setting_find(key), v, NULL);
}
static int valid_rise_c(const char *v)     { return valid_gate("cool_flow_rise", v); }
static int valid_check_s(const char *v)    { return valid_gate("cool_flow_check_s", v); }
static int valid_temp_max(const char *v)   { return valid_gate("cool_temp_max", v); }
static int valid_temp_resume(const char *v){ return valid_gate("cool_temp_resume", v); }
static int valid_temp_critical(const char *v){ return valid_gate("cool_temp_critical_c", v); }
static int valid_exhaust_rpm(const char *v) { return valid_gate("cool_tach_exhaust_min_rpm", v); }
static int valid_intake_rpm(const char *v)  { return valid_gate("cool_tach_intake_min_rpm", v); }
static int valid_air_rpm(const char *v)     { return valid_gate("cool_tach_air_assist_min_rpm", v); }
static int valid_purge_cur(const char *v)   { return valid_gate("cool_purge_min_current", v); }
static int valid_grace_s(const char *v)     { return valid_gate("cool_fan_grace_s", v); }
static int valid_heater_pct(const char *v) { return valid_range(v, 0, 100); }
static int valid_recheck_s(const char *v)  { return valid_range(v, 0, 3600); }
static int valid_confirm_s(const char *v)  { return valid_range(v, 60, 3600); }
static int valid_cool_s(const char *v)     { return valid_range(v, 0, 1800); }

/* GRBL-mode tunables, read by the controller from the same file. The
 * button wait runs with the laser latch unlocked and the controller
 * refuses an unbounded value, so the range here matches its clamp; the
 * disarm grace and the rail settle are bounded to what the machine can
 * mean by them. */
static int valid_button_s(const char *v)   { return valid_range(v, 1, 3600); }
static int valid_disarm_s(const char *v)   { return valid_range(v, 1, 3600); }
static int valid_settle_s(const char *v)   { return valid_range(v, 0, 30); }

/* The lid lamp's idle level (PWM 0-255; unset = 236). Applied live. */
static int valid_lamp(const char *v)       { return valid_range(v, 0, 255); }

/* Cloud-mode print pause: pulse ticks retraced (laser off) on the button
 * press, and the laser-off lead the resume runs before re-enabling
 * (unset = the factory's 2000 / 1950). The kernel reserves 32 KiB of
 * ring for the backtrack; both stay well inside it. */
static int valid_ticks(const char *v)      { return valid_range(v, 0, 30000); }

/* Cloud-mode download guards: bytes of pulse body the client will hold in
 * memory (unset = 32 MiB warn, 128 MiB refuse; 0 lifts either). The body is
 * the job as the service compressed it, tens to one, so these bound this
 * machine's memory and not the length of a job: a job longer than the ring
 * is fed as it plays. A gigabyte is well past any real ceiling and is here
 * so a typo cannot ask for one. */
static int valid_pulse_bytes(const char *v) { return valid_range(v, 0, 1073741824); }

/* GRBL mode: what a lid or interlock open does to a running job -
 * "cancel" (the factory's abort + return to the job start; unset = this)
 * or "hold" (stock grblHAL door hold, cycle start resumes). */
static int valid_lid_policy(const char *v) { return !strcmp(v, "cancel") || !strcmp(v, "hold"); }

/* Logging: per-logger disk and remote levels (each off|error|warning|
 * notice|info|debug) and the remote syslog target. Read at boot by
 * `forgectrl --render-syslog` (rsyslog rules) and by each process for
 * its own emit level, so a change applies at the next reboot. */

static const struct {
    const char *key;
    int (*valid)(const char *);
    int secret;
} setting_defs[] = {
    { "controller_mode",        valid_controller_mode, 0 },
    { "homing_mode",            valid_homing_mode, 0 },
    { "gfcloud_home_x",         valid_mm,          0 },
    { "gfcloud_home_y",         valid_mm,          0 },
    { "gfcloud_home_z",         valid_mm,          0 },
    { "gfcloud_home_timeout_s", valid_timeout,     0 },
    { "gf_serial",              valid_serial,      0 },
    { "gf_password",            valid_password,    1 },
    { "ui_units",               valid_units,       0 },
    { "wifi_country",           valid_country,     0 },
    { "cool_flow_rise",         valid_rise_c,      0 },
    { "cool_flow_heater_pct",   valid_heater_pct,  0 },
    { "cool_flow_check_s",      valid_check_s,     0 },
    { "cool_recheck_s",         valid_recheck_s,   0 },
    { "cool_confirm_max_s",     valid_confirm_s,   0 },
    { "cool_temp_max",          valid_temp_max,    0 },
    { "cool_temp_resume",       valid_temp_resume, 0 },
    { "cool_temp_critical_c",   valid_temp_critical, 0 },
    { "cool_cooldown_s",        valid_cool_s,      0 },
    { "cool_cooldown_max_s",    valid_cool_s,      0 },
    { "cool_tach_exhaust_min_rpm",    valid_exhaust_rpm, 0 },
    { "cool_tach_intake_min_rpm",     valid_intake_rpm,  0 },
    { "cool_tach_air_assist_min_rpm", valid_air_rpm,     0 },
    { "cool_purge_min_current",       valid_purge_cur,   0 },
    { "cool_fan_grace_s",             valid_grace_s,     0 },
    { "laser_button_timeout_s", valid_button_s,    0 },
    { "laser_disarm_s",         valid_disarm_s,    0 },
    { "rail_settle_s",          valid_settle_s,    0 },
    { "lid_lamp_idle",          valid_lamp,        0 },
    { "cloud_pause_backtrack_ticks", valid_ticks,  0 },
    { "cloud_resume_lead_ticks",     valid_ticks,  0 },
    { "pulse_warn_threshold_bytes",   valid_pulse_bytes, 0 },
    { "pulse_reject_threshold_bytes", valid_pulse_bytes, 0 },
    { "lid_policy",             valid_lid_policy,  0 },
    { "log_forgectrl_disk",     logs_valid_level,  0 },
    { "log_forgectrl_remote",   logs_valid_level,  0 },
    { "log_grblhal_disk",       logs_valid_level,  0 },
    { "log_grblhal_remote",     logs_valid_level,  0 },
    { "log_gfcloud_disk",       logs_valid_level,  0 },
    { "log_gfcloud_remote",     logs_valid_level,  0 },
    { "log_gfhome_disk",        logs_valid_level,  0 },
    { "log_gfhome_remote",      logs_valid_level,  0 },
    { "log_kernel_disk",        logs_valid_level,  0 },
    { "log_kernel_remote",      logs_valid_level,  0 },
    { "log_system_disk",        logs_valid_level,  0 },
    { "log_system_remote",      logs_valid_level,  0 },
    { "syslog_server",          logs_valid_server, 0 },
    { "syslog_port",            logs_valid_port,   0 },
    { "syslog_proto",           logs_valid_proto,  0 },
};
#define N_SETTINGS (sizeof(setting_defs) / sizeof(*setting_defs))

/* The settings as text for the log export: one "key = value" per line,
 * secrets shown only as set/unset. */
static void settings_snapshot(FILE *out)
{
    char val[128];
    for (size_t i = 0; i < N_SETTINGS; i++) {
        int have = settings_get(setting_defs[i].key, val, sizeof(val)) == 0 &&
                   setting_defs[i].valid(val);
        if (setting_defs[i].secret)
            fprintf(out, "%s = %s\n", setting_defs[i].key,
                    have ? "<set>" : "<unset>");
        else
            fprintf(out, "%s = %s\n", setting_defs[i].key, have ? val : "");
    }
}

/* WiFi radio policy, applied at startup and whenever the wifi_country
 * setting changes. The stored region (unset = "00", the world domain)
 * goes to cfg80211 as the user regulatory hint; power save is pinned
 * off - on a mains-powered machine it only adds latency. */
static void apply_wifi(int set_region)
{
    char cc[8], cmd[48];
    if (settings_get("wifi_country", cc, sizeof(cc)) != 0 ||
        !valid_country(cc))
        snprintf(cc, sizeof(cc), "00");
    /* Reload the database first: when cfg80211 initialized before the
     * rootfs was mounted, its boot-time regulatory.db load failed and
     * stays failed until an explicit reload. */
    (void)!system("iw reg reload >/dev/null 2>&1");
    /* The kernel already defaults to the world domain; hinting 00 on
     * top of it only produces a cosmetic "country 98" intersection.
     * 00 is set only to revert from a previously applied region. */
    if (set_region || strcmp(cc, "00")) {
        snprintf(cmd, sizeof(cmd), "iw reg set %s >/dev/null 2>&1", cc);
        if (system(cmd) != 0)
            fflog(LOG_ERR, "iw reg set %s failed", cc);
    }
    if (system("iw dev wlan0 set power_save off >/dev/null 2>&1") != 0)
        fflog(LOG_ERR, "wifi power_save off failed");
}

/* Machine identity, derived from the i.MX6 OCOTP fuses exactly like
 * the factory firmware: the serial is fuse word HW_OCOTP_MAC0 (nvmem
 * word 34 on the mainline imx-ocotp driver; hex text on the legacy
 * fsl_otp path), and the factory hostname is that serial encoded
 * base-23 over the alphabet BCDFGHJKMQRTVWXY2346789, up to six
 * characters, split XXX-YYY. The GUI always identifies the machine by
 * this fuse identity - the gf_serial setting overrides only what is
 * sent to the Glowforge cloud. */
unsigned long fuse_serial(void)
{
    static unsigned long cached;
    static int tried;
    if (!tried) {
        tried = 1;
        FILE *f = fopen("/sys/bus/nvmem/devices/imx-ocotp0/nvmem", "rb");
        if (f) {
            unsigned char w[4];
            if (fseek(f, 136, SEEK_SET) == 0 && fread(w, 1, 4, f) == 4)
                cached = (unsigned long)w[0] | ((unsigned long)w[1] << 8) |
                         ((unsigned long)w[2] << 16) |
                         ((unsigned long)w[3] << 24);
            fclose(f);
        } else if ((f = fopen("/sys/fsl_otp/HW_OCOTP_MAC0", "r")) != NULL) {
            if (fscanf(f, "%lx", &cached) != 1)
                cached = 0;
            fclose(f);
        }
    }
    return cached;
}

/* Fuse password: the eight SRK words (nvmem bank 3 words 0-7, byte
 * offset 96) as 64 hex digits - what the machine signs in to the
 * Glowforge service with. Read on demand only (the fuse-identity
 * viewer), never included in routine polls. */
int fuse_password(char *buf, size_t len)
{
    buf[0] = '\0';
    if (len < 65)
        return -1;
    FILE *f = fopen("/sys/bus/nvmem/devices/imx-ocotp0/nvmem", "rb");
    if (f) {
        unsigned char w[32];
        int ok = fseek(f, 96, SEEK_SET) == 0 && fread(w, 1, 32, f) == 32;
        fclose(f);
        if (!ok)
            return -1;
        for (int i = 0; i < 8; i++)
            snprintf(buf + i * 8, 9, "%08lx",
                     (unsigned long)w[i * 4] |
                     ((unsigned long)w[i * 4 + 1] << 8) |
                     ((unsigned long)w[i * 4 + 2] << 16) |
                     ((unsigned long)w[i * 4 + 3] << 24));
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        char path[48];
        unsigned long v;
        snprintf(path, sizeof(path), "/sys/fsl_otp/HW_OCOTP_SRK%d", i);
        if ((f = fopen(path, "r")) == NULL)
            return -1;
        int ok = fscanf(f, "%lx", &v) == 1;
        fclose(f);
        if (!ok)
            return -1;
        snprintf(buf + i * 8, 9, "%08lx", v);
    }
    return 0;
}

void machine_id(char *buf, size_t len)
{
    static char cached[16];
    if (!cached[0]) {
        unsigned long serial = fuse_serial();
        if (serial) {
            static const char alpha[] = "BCDFGHJKMQRTVWXY2346789";
            char enc[8];
            int n = 0;
            while (serial > 0 && n < 6) {
                enc[n++] = alpha[serial % 23];
                serial /= 23;
            }
            int o = 0;
            for (int i = n - 1; i >= 0; i--) {
                cached[o++] = enc[i];
                if (i == n - 3 && i > 0)
                    cached[o++] = '-';
            }
            cached[o] = '\0';
        }
    }
    snprintf(buf, len, "%s", cached);
}

/* /etc/forgefirm-version: written by the image build (release version,
 * or build timestamp + dev tag). Sanitized for direct JSON embedding. */
static void read_fw_version(char *buf, size_t len)
{
    buf[0] = '\0';
    FILE *f = fopen("/etc/forgefirm-version", "r");
    if (!f)
        return;
    if (!fgets(buf, (int)len, f))
        buf[0] = '\0';
    fclose(f);
    size_t o = 0;
    for (size_t i = 0; buf[i]; i++)
        if (buf[i] >= ' ' && buf[i] != '"' && buf[i] != '\\')
            buf[o++] = buf[i];
    while (o > 0 && buf[o - 1] == ' ')
        o--;
    buf[o] = '\0';
}

/* Append into a fixed buffer, keeping the running offset within bounds:
 * snprintf returns the would-have-written length, so an unclamped
 * accumulator can run past the buffer and underflow the next
 * `size - off`. Clamped here so every subsequent append is a safe no-op
 * once the buffer is full. */
static void append(char *buf, size_t size, size_t *off, const char *fmt, ...)
{
    if (*off >= size)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, size - *off, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    *off += (size_t)n;
    if (*off >= size)
        *off = size - 1;                /* truncated; keep off in range */
}

/* A gate setting's value as the file holds it, or its default. */
static double setting_gate_value(const gate_setting_t *g, void *ctx)
{
    (void)ctx;
    char v[128];
    double out;
    if (settings_get(g->key, v, sizeof(v)) == 0 && gate_parse(g, v, &out))
        return out;
    return g->def;
}

static int reply_settings(struct _u_response *res)
{
    char body[6144], val[128], mid[16], fwver[48];
    size_t off = 0;

    read_fw_version(fwver, sizeof(fwver));
    machine_id(mid, sizeof(mid));

    append(body, sizeof(body), &off, "{");
    for (size_t i = 0; i < N_SETTINGS; i++) {
        /* A value that fails its own validator (hand-edited file) is
         * reported as unset rather than leaking arbitrary bytes into
         * the JSON. */
        int have = settings_get(setting_defs[i].key, val, sizeof(val)) == 0 &&
                   setting_defs[i].valid(val);
        if (setting_defs[i].secret)
            append(body, sizeof(body), &off, "\"%s_set\":%s,",
                   setting_defs[i].key, have ? "true" : "false");
        else
            append(body, sizeof(body), &off, "\"%s\":\"%s\",",
                   setting_defs[i].key, have ? val : "");
    }
    /* The gate settings with their ranges, bands and states, from the
     * file's values (the default where unset): what the panel renders
     * its warnings from, and the record of any gate that is off by
     * value. The engine reports the same from its resolved tunables in
     * /cool/status and /status. */
    char gates[2048];
    if (gates_json(gates, sizeof(gates), setting_gate_value, NULL) > 0)
        append(body, sizeof(body), &off, "\"gates\":%s,", gates);
    append(body, sizeof(body), &off,
           "\"version\":\"%s\",\"machine_id\":\"%s\"}", fwver, mid);
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_settings_get(const struct _u_request *req,
                           struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    return reply_settings(res);
}

static int cb_machine_status(const struct _u_request *req,
                             struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[1536], gates[128];
    cool_gates_off_json(gates, sizeof(gates));
    char extra[160];
    snprintf(extra, sizeof(extra), "\"gates_off\":%s", gates);
    machine_status_json(body, sizeof(body), extra);
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static const char *setting_param(const struct _u_request *req,
                                 const char *key)
{
    const char *v = u_map_get(req->map_post_body, key);
    return v ? v : u_map_get(req->map_url, key);
}

/* Effective value of a cooling temperature key for cross-field checks:
 * the request value if this POST sets it, else the persisted value, else
 * the compiled default. Returns 0 and leaves *out untouched on a key
 * that is neither in the request nor stored (its default stands). */
static int effective_temp(const struct _u_request *req, const char *key,
                          double dflt, double *out)
{
    const char *v = setting_param(req, key);
    char stored[128];
    if (v && v[0])
        *out = strtod(v, NULL);
    else if (v && !v[0])
        *out = dflt;                    /* this POST clears it */
    else if (settings_get(key, stored, sizeof(stored)) == 0 && stored[0])
        *out = strtod(stored, NULL);
    else
        *out = dflt;
    return 0;
}

static int cb_settings_post(const struct _u_request *req,
                            struct _u_response *res, void *user_data)
{
    (void)user_data;
    int present = 0;

    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;

    /* Settings are locked whenever the machine is not idle: the
     * controller and the homing runner both read this file mid-run.
     * A running diagnostic owns the hardware and locks them too. */
    if (diag_running())
        return reply_error(res, 409,
            "a diagnostic is running - settings are locked");
    if (!machine_is_idle())
        return reply_error(res, 409,
            "machine is not idle - settings are locked");

    /* Validate the whole request before writing anything. */
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const char *v = setting_param(req, setting_defs[i].key);
        if (!v)
            continue;
        present++;
        if (v[0] && !setting_defs[i].valid(v)) {
            char err[96];
            snprintf(err, sizeof(err), "invalid value for %s",
                     setting_defs[i].key);
            return reply_error(res, 400, err);
        }
    }
    if (!present)
        return reply_error(res, 400, "no known setting in request");

    /* Cross-field cooling safety: the resume ceiling must not sit at or
     * above the over-temp ceiling, or the hysteresis inverts and the
     * machine can resume into an over-temperature it never leaves. The
     * per-key validators already cap each to a bounded range; this pins
     * their relationship across a multi-key POST. */
    double tmax, tresume, tcrit;
    effective_temp(req, "cool_temp_max", 33.0, &tmax);
    effective_temp(req, "cool_temp_resume", 31.0, &tresume);
    effective_temp(req, "cool_temp_critical_c", 38.0, &tcrit);
    if (tresume >= tmax)
        return reply_error(res, 400,
            "cool_temp_resume must be below cool_temp_max");
    /* The critical line is the fail tier above the pause tier; at or
     * below the ceiling it would fail a job the ceiling meant to pause.
     * A ceiling at its off end is no ceiling (every gate is off by value
     * on its own), so the line is only held above a ceiling that gates. */
    const gate_setting_t *ceil = gate_setting_find("cool_temp_max");
    int ceiling_off = ceil && ceil->off_end > 0 && tmax >= ceil->hi;
    if (!ceiling_off && tcrit <= tmax)
        return reply_error(res, 400,
            "cool_temp_critical_c must be above cool_temp_max");

    /* One atomic write for the whole request: no reader (grblHAL at $H,
     * gfhome at session start) can observe it half-applied, and a
     * concurrent writer cannot interleave between the keys. */
    const char *keys[N_SETTINGS], *vals[N_SETTINGS];
    size_t nset = 0;
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const char *v = setting_param(req, setting_defs[i].key);
        if (!v)
            continue;
        keys[nset] = setting_defs[i].key;
        vals[nset] = v;
        nset++;
    }
    if (settings_set_many(keys, vals, nset) != 0)
        return reply_error(res, 500, "cannot write settings file");
    for (size_t i = 0; i < N_SETTINGS; i++) {
        const char *v = setting_param(req, setting_defs[i].key);
        if (!v)
            continue;
        fflog(LOG_NOTICE, "%s %s", setting_defs[i].key,
              !v[0] ? "cleared" :
              setting_defs[i].secret ? "set" : v);
    }
    if (setting_param(req, "wifi_country"))
        apply_wifi(1);
    if (setting_param(req, "lid_lamp_idle"))
        cam_lamp_apply_idle();
    return reply_settings(res);
}

/* Manual emergency lever (the controller init scripts route their
 * stop/start here): stop the active controller and HOLD supervision
 * suspended - a bare pkill would be safed and respawned seconds later.
 * Deliberately NOT idle-gated: an emergency stop must work mid-job (the
 * supervisor's exit safing writes cnc/stop + laser latch). */
static int cb_controller_stop(const struct _u_request *req,
                              struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    if (super_controller_stop() != 0)
        return reply_error(res, 500, "controller did not stop");
    ulfius_set_string_body_response(res, 200, "{\"stopped\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_controller_start(const struct _u_request *req,
                               struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    super_controller_start();
    ulfius_set_string_body_response(res, 200, "{\"started\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* The burned-in identity, on demand for the GF Cloud tab's viewer.
 * The values are irrevocable (fuses), so they are fetched only when
 * the operator explicitly asks to see them. */
static int cb_fuse_identity(const struct _u_request *req,
                            struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    /* The SRK password is irrevocable and never rotates - reveal it only
     * to someone physically at the machine holding the button. */
    if (!operator_present())
        return reply_error(res, 403,
            "hold the machine button to reveal the fuse identity");
    char body[192], mid[16], pw[65];
    unsigned long serial = fuse_serial();
    machine_id(mid, sizeof(mid));
    fuse_password(pw, sizeof(pw));
    if (serial)
        snprintf(body, sizeof(body),
                 "{\"serial\":\"%lu\",\"hostname\":\"%s\","
                 "\"password\":\"%s\"}", serial, mid, pw);
    else
        snprintf(body, sizeof(body),
                 "{\"serial\":\"\",\"hostname\":\"\",\"password\":\"\"}");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    return U_CALLBACK_CONTINUE;
}

/* ---------------------------------------------------------------- mode */

static int cb_mode_get(const struct _u_request *req,
                       struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[128];
    super_status_json(body, sizeof(body));
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_mode_post(const struct _u_request *req,
                        struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *mode = setting_param(req, "controller");
    if (!mode)
        return reply_error(res, 400, "controller is required");
    char err[96];
    if (super_mode_switch(mode, err, sizeof(err)) != 0)
        return reply_error(res, strstr(err, "must be") ? 400 : 409, err);
    return cb_mode_get(req, res, NULL);
}

/* ------------------------------------------------------------- cooling */

/* Level-triggered job-state report from the active controller (~1 Hz).
 * Query/form parameters: mode=idle|run|cooldown, armed=0|1, and an
 * optional per-job run fan profile (air_assist, exhaust, intake). */
static int cb_cool_state(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    (void)user_data;
    /* The thermal-safety report channel: only the controller on this
     * same host writes it. Restricting it to a loopback peer keeps a LAN
     * client from spoofing a stand-down that drops the exhaust mid-cut. */
    if (!auth_loopback_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *mode = setting_param(req, "mode");
    if (!mode)
        return reply_error(res, 400, "mode is required");

    const char *v = setting_param(req, "armed");
    int armed = v && atoi(v) != 0;
    long duty[3] = {-1, -1, -1};
    static const char *duty_key[3] = {"air_assist", "exhaust", "intake"};
    for (int i = 0; i < 3; i++)
        if ((v = setting_param(req, duty_key[i])) != NULL)
            duty[i] = atol(v);

    /* The job's limits, when the report carries them (cloud mode): each
     * a number the engine takes only where it is stricter than its own;
     * absent or unparsable reads as absent. */
    cool_limits_t lim = {-1, -1, -1, -1, -1};
    static const char *lim_key[5] = {"coolant_max_c", "coolant_min_c",
                                     "exhaust_min_rpm", "intake_min_rpm",
                                     "air_assist_min_rpm"};
    double *lim_val[5] = {&lim.coolant_max_c, &lim.coolant_min_c,
                          &lim.exhaust_min_rpm, &lim.intake_min_rpm,
                          &lim.air_assist_min_rpm};
    for (int i = 0; i < 5; i++) {
        if ((v = setting_param(req, lim_key[i])) == NULL)
            continue;
        char *end;
        double d = strtod(v, &end);
        if (end != v && *end == '\0')
            *lim_val[i] = d;
    }

    if (cool_state_report(mode, armed, duty[0], duty[1], duty[2], &lim) != 0)
        return reply_error(res, 400, "mode must be idle, run or cooldown");
    ulfius_set_string_body_response(res, 200, "{\"ok\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_cool_status(const struct _u_request *req,
                          struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[COOL_STATUS_JSON_MAX];
    if (cool_status_json(body, sizeof(body)) < 0)
        return reply_error(res, 500, "cool status document too long");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* --------------------------------------------------------- diagnostics */

static int cb_diag_start(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    /* A diagnostic seizes the thermal hardware; refuse it while a
     * firmware job is mid-flight (the update path does not otherwise
     * share diag's busy check). */
    if (update_job_running())
        return reply_error(res, 409, "an update job is running");
    switch (diag_start((const char *)user_data)) {
    case 0:
        ulfius_set_string_body_response(res, 202, "{\"started\":true}");
        ulfius_add_header_to_response(res, "Content-Type",
                                      "application/json");
        return U_CALLBACK_CONTINUE;
    case -1:
        return reply_error(res, 409, "a diagnostic is already running");
    case -2:
        return reply_error(res, 409, "machine is not idle");
    default:
        return reply_error(res, 400, "unknown diagnostic");
    }
}

static int cb_diag_abort(const struct _u_request *req,
                         struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    diag_abort();
    ulfius_set_string_body_response(res, 200, "{\"aborting\":true}");
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int cb_diag_status(const struct _u_request *req,
                          struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char body[4096];
    diag_status_json(body, sizeof(body));
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

/* The panel: the embedded gzip bundle inflated once, with the per-machine
 * token spliced in place of the __FFTOKEN__ placeholder. Serving the
 * token inside the page (rather than from an endpoint any LAN client
 * could call) is what lets the origin checks keep it out of a rebinding
 * attacker's reach. The splice happens on the inflated text, never
 * inside the compressed stream. */
static const char *panel_html(void)
{
    static char *page;
    static const char fallback[] =
        "<!doctype html><title>ForgeFIRM</title>"
        "forgectrl: the panel could not be unpacked";
    if (page)
        return page;

    char *html = malloc((size_t)index_html_len + 1);
    if (!html)
        return fallback;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (inflateInit2(&zs, 15 + 16) != Z_OK) {   /* 16: gzip wrapper */
        free(html);
        return fallback;
    }
    zs.next_in = (Bytef *)index_html_gz;
    zs.avail_in = index_html_gz_len;
    zs.next_out = (Bytef *)html;
    zs.avail_out = index_html_len;
    int rc = inflate(&zs, Z_FINISH);
    size_t n = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END) {
        free(html);
        return fallback;
    }
    html[n] = '\0';

    const char *mark = strstr(html, "__FFTOKEN__");
    const char *tok = auth_token();
    if (!mark || !tok[0]) {
        page = html;                    /* no token: serve inert page */
        return page;
    }
    size_t pre = (size_t)(mark - html);
    size_t tlen = strlen(tok);
    size_t total = n - strlen("__FFTOKEN__") + tlen + 1;
    page = malloc(total);
    if (!page) {
        page = html;
        return page;
    }
    memcpy(page, html, pre);
    memcpy(page + pre, tok, tlen);
    strcpy(page + pre + tlen, mark + strlen("__FFTOKEN__"));
    free(html);
    return page;
}

/* ---------------------------------------------------------------- logs */

static int cb_logs_list(const struct _u_request *req,
                        struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    char *body = logs_list_json();
    if (!body)
        return reply_error(res, 500, "out of memory");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    free(body);
    return U_CALLBACK_CONTINUE;
}

/* Log content is token-gated like a write: it can carry network
 * addresses and protocol detail the open status endpoints never do. */
static int cb_logs_tail(const struct _u_request *req,
                        struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *name = u_map_get(req->map_url, "name");
    const char *l = u_map_get(req->map_url, "lines");
    const char *f = u_map_get(req->map_url, "from");
    long lines = l ? atol(l) : 200;
    long long from = f ? atoll(f) : -1;
    if (!name)
        return reply_error(res, 400, "name required");
    char *body = logs_tail_json(name, lines, from);
    if (!body)
        return reply_error(res, 404, "unknown logger");
    ulfius_set_string_body_response(res, 200, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    free(body);
    return U_CALLBACK_CONTINUE;
}

static ssize_t export_stream_cb(void *cls, uint64_t pos, char *buf,
                                size_t max)
{
    (void)pos;
    ssize_t n = logs_export_read(cls, buf, max);
    if (n < 0)
        return U_STREAM_ERROR;
    if (n == 0)
        return U_STREAM_END;
    return n;
}

static void export_stream_free(void *cls)
{
    logs_export_end(cls);
}

/* Bundle every logger's files plus a system snapshot into a tar.gz.
 * Sanitized by default (for public issue reports); ?sanitize=0 keeps
 * everything. Streams; the staging area is removed when the stream
 * ends, however it ends. */
static int cb_logs_export(const struct _u_request *req,
                          struct _u_response *res, void *user_data)
{
    (void)user_data;
    if (!auth_write_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *sv = setting_param(req, "sanitize");
    int sanitize = !(sv && (!strcmp(sv, "0") || !strcmp(sv, "false") ||
                            !strcmp(sv, "no")));
    char err[160];
    logs_export_t *e = logs_export_begin(sanitize, settings_snapshot, err,
                                        sizeof(err));
    if (!e)
        return reply_error(res, !strcmp(err, "busy") ? 409 : 500, err);
    char fn[96], ts[24];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    snprintf(fn, sizeof(fn), "attachment; filename=\"forgefirm-logs-%s%s"
             ".tar.gz\"", ts, sanitize ? "" : "-full");
    ulfius_add_header_to_response(res, "Content-Type", "application/gzip");
    ulfius_add_header_to_response(res, "Content-Disposition", fn);
    ulfius_add_header_to_response(res, "Cache-Control", "no-store");
    ulfius_set_stream_response(res, 200, export_stream_cb, export_stream_free,
                               U_STREAM_SIZE_UNKNOWN, 16 * 1024, e);
    return U_CALLBACK_CONTINUE;
}

/* "/" serves the UI (src/ui/), plus the mjpg-streamer-compatible
 * ?action=stream / ?action=snapshot aliases many clients expect. */
static int cb_root(const struct _u_request *req, struct _u_response *res,
                   void *user_data)
{
    (void)user_data;
    if (!auth_read_ok(req, res))
        return U_CALLBACK_COMPLETE;
    const char *action = u_map_get(req->map_url, "action");
    if (action) {
        if (!strcmp(action, "stream"))
            return do_stream(CAM_LID, res);
        if (!strcmp(action, "snapshot"))
            return do_snapshot(CAM_LID, 1, SNAP_Q_DEF, -1, res);
        return reply_error(res, 400, "unknown action");
    }
    ulfius_set_string_body_response(res, 200, panel_html());
    ulfius_add_header_to_response(res, "Content-Type", "text/html");
    return U_CALLBACK_CONTINUE;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    unsigned port = DEFAULT_PORT;
    const char *v = getenv("FORGECTRL_PORT");
    if (v && atoi(v) > 0 && atoi(v) < 65536)
        port = (unsigned)atoi(v);

    /* Boot-time helper: render the rsyslog rules from the settings and
     * leave. Runs before rsyslog (and before this daemon) starts. */
    if (argc > 1 && !strcmp(argv[1], "--render-syslog")) {
        char err[160];
        if (logs_render(err, sizeof(err)) != 0) {
            fprintf(stderr, "forgectrl: render-syslog: %s\n", err);
            return 1;
        }
        return 0;
    }
    if (argc > 1) {
        fprintf(stderr, "usage: forgectrl [--render-syslog]\n");
        return 2;
    }

    fflog_init("forgectrl");

    /* Stay well below the motion feeder (SCHED_FIFO) and the controller;
     * best effort. */
    (void)!nice(5);

    /* The daemon is the dead-man for hung controllers and the sole
     * cooling-hardware writer: under memory pressure it must outlive
     * the processes it supervises. Controllers respawn at -500 (see
     * super.c), so they are reclaimed first and the daemon safes and
     * respawns them. */
    int ofd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (ofd >= 0) {
        (void)!write(ofd, "-900", 4);
        close(ofd);
    }

    /* Raise the descriptor ceiling: the daemon is thread-per-connection,
     * so a connection flood must not exhaust the fd table and make
     * sysfs reads fail - machine_is_idle() fails closed on that now, but
     * a higher ceiling keeps the daemon serving through the flood. */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < 4096) {
        rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        (void)setrlimit(RLIMIT_NOFILE, &rl);
    }

    auth_init();
    cam_engine_init();
    diag_init();
    cool_init();
    super_init();
    update_init();
    apply_wifi(0);
    cam_lamp_apply_idle();

    struct _u_instance inst;
    /* Dual-stack listener: one socket serves IPv4 and IPv6 clients. */
    if (ulfius_init_instance_ipv6(&inst, port, NULL, U_USE_ALL, NULL) != U_OK) {
        fflog(LOG_ERR, "ulfius init failed");
        return 1;
    }
    ulfius_add_endpoint_by_val(&inst, "GET", "/", NULL, 0, &cb_root, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/cam/stream", NULL, 0,
                               &cb_stream, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/cam/h264", NULL, 0,
                               &cb_h264, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/cam/snapshot", NULL, 0,
                               &cb_snapshot, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/cam/status", NULL, 0,
                               &cb_status, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/settings", NULL, 0,
                               &cb_settings_get, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/settings", NULL, 0,
                               &cb_settings_post, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/status", NULL, 0,
                               &cb_machine_status, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/fuse-identity", NULL, 0,
                               &cb_fuse_identity, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/mode", NULL, 0,
                               &cb_mode_get, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/mode", NULL, 0,
                               &cb_mode_post, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/controller/stop", NULL, 0,
                               &cb_controller_stop, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/controller/start", NULL, 0,
                               &cb_controller_start, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/cool/state", NULL, 0,
                               &cb_cool_state, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/cool/status", NULL, 0,
                               &cb_cool_status, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/diag/flow-verify", NULL, 0,
                               &cb_diag_start, "flow-verify");
    ulfius_add_endpoint_by_val(&inst, "POST", "/diag/flow-calibrate", NULL,
                               0, &cb_diag_start, "flow-calibrate");
    ulfius_add_endpoint_by_val(&inst, "POST", "/diag/abort", NULL, 0,
                               &cb_diag_abort, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/diag/status", NULL, 0,
                               &cb_diag_status, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/slots", NULL, 0,
                               &cb_slots, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/boot", NULL, 0,
                               &cb_boot_select, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/system/reboot", NULL, 0,
                               &cb_system_reboot, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/update/check", NULL, 0,
                               &cb_update_check, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/update/download", NULL, 0,
                               &cb_update_download, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/update/apply", NULL, 0,
                               &cb_update_apply, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/update/upload", NULL, 0,
                               &cb_update_upload, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/restore/factory", NULL, 0,
                               &cb_restore_factory, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/update/status", NULL, 0,
                               &cb_update_status, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/logs", NULL, 0,
                               &cb_logs_list, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/logs/tail", NULL, 0,
                               &cb_logs_tail, NULL);
    ulfius_add_endpoint_by_val(&inst, "POST", "/logs/export", NULL, 0,
                               &cb_logs_export, NULL);
    ulfius_set_upload_file_callback_function(&inst, &update_upload_sink,
                                             NULL);

    if (ulfius_start_framework(&inst) != U_OK) {
        fflog(LOG_ERR, "cannot start HTTP on port %u", port);
        ulfius_clean_instance(&inst);
        return 1;
    }
    fflog(LOG_NOTICE, "listening on port %u", port);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    while (!quit)
        pause();

    fflog(LOG_NOTICE, "shutting down");
    ulfius_stop_framework(&inst);
    ulfius_clean_instance(&inst);
    super_shutdown();
    cool_shutdown();
    cam_engine_shutdown();
    return 0;
}

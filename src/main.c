/*
 * main.c - forgectrl: ForgeFIRM system control daemon
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * HTTP service (ulfius) exposing the Glowforge cameras as MJPEG:
 *
 *   GET /                        the machine control panel (ui.c)
 *   GET /?action=stream          mjpg-streamer-compatible stream (lid)
 *   GET /?action=snapshot        mjpg-streamer-compatible snapshot (lid)
 *   GET /cam/stream?cam=lid|head            multipart MJPEG, 1296x972
 *   GET /cam/snapshot?cam=&res=full|half&q=&lamp=  single JPEG (full res;
 *                                lamp overrides the scene lamp for the shot)
 *   GET /cam/status                         JSON engine status
 *   GET /status                             JSON machine operational status
 *   GET /settings                           JSON machine settings
 *   POST /settings?key=value                update machine settings
 *   GET /fuse-identity                      burned-in identity (on demand)
 *
 * The two cameras share the hardware mux; the newest request wins it. A
 * STREAM request for the other camera preempts the current stream
 * clients (their streams end cleanly - viewers freeze on the last frame)
 * and switches. A SNAPSHOT of the other camera does not switch: the
 * engine borrows the mux for one frame and the stream freezes briefly.
 * Environment: FORGECTRL_PORT (8080), FORGECTRL_STREAM_Q (75),
 * FORGECTRL_LAMP (132), FORGECTRL_STREAM_FPS (0 = sensor max).
 *
 * ulfius runs libmicrohttpd in thread-per-connection mode, so each stream
 * callback may block waiting for the next frame.
 */
#define _GNU_SOURCE
#include "auth.h"
#include "cam.h"
#include "cool.h"
#include "diag.h"
#include "settings.h"
#include "status.h"
#include "super.h"
#include "ui.h"
#include "update.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <ulfius.h>
#include <unistd.h>

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

static int do_stream(cam_id_t cam, struct _u_response *res)
{
    char err[256];
    cam_client_t *cl = cam_client_open(cam, err, sizeof(err));
    if (!cl)
        return reply_error(res, strstr(err, "busy") ? 409 : 503, err);

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
        return reply_error(res, strstr(err, "busy") ? 409 : 503, err);
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
    char body[320];
    snprintf(body, sizeof(body),
             "{\"running\":%s,\"cam\":\"%s\",\"clients\":%d,"
             "\"frames\":%llu,\"fps\":%.1f,\"fps_cap\":%.1f,"
             "\"encoder\":\"%s\",\"buffers\":\"%s\","
             "\"stream\":{\"width\":1296,\"height\":972},"
             "\"snapshot\":{\"width\":2592,\"height\":1944}}",
             st.running ? "true" : "false", cam_name(st.cam), st.clients,
             (unsigned long long)st.seq, st.fps, st.fps_cap,
             st.vpu ? "vpu" : "software",
             st.cached ? "cached" : "uncached");
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

/* The flow-rise threshold must stay below the no-flow rise the bench
 * measured (~16 C over the check window): above it the interrogation can
 * never fault, so the machine would fire with the pump stopped. Capped
 * well under that. The over-temp ceiling is capped near the factory
 * window (~33 C) so it cannot be lifted into a range where the tube runs
 * hot without faulting. */
static int valid_rise_c(const char *v)     { return valid_range(v, 1, 15); }
static int valid_heater_pct(const char *v) { return valid_range(v, 0, 100); }
static int valid_check_s(const char *v)    { return valid_range(v, 0, 300); }
static int valid_recheck_s(const char *v)  { return valid_range(v, 0, 3600); }
static int valid_confirm_s(const char *v)  { return valid_range(v, 60, 3600); }
static int valid_temp_c(const char *v)     { return valid_range(v, 5, 38); }
static int valid_cool_s(const char *v)     { return valid_range(v, 0, 1800); }

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
    { "cool_temp_max",          valid_temp_c,      0 },
    { "cool_temp_resume",       valid_temp_c,      0 },
    { "cool_cooldown_s",        valid_cool_s,      0 },
    { "cool_cooldown_max_s",    valid_cool_s,      0 },
};
#define N_SETTINGS (sizeof(setting_defs) / sizeof(*setting_defs))

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
    system("iw reg reload >/dev/null 2>&1");
    /* The kernel already defaults to the world domain; hinting 00 on
     * top of it only produces a cosmetic "country 98" intersection.
     * 00 is set only to revert from a previously applied region. */
    if (set_region || strcmp(cc, "00")) {
        snprintf(cmd, sizeof(cmd), "iw reg set %s >/dev/null 2>&1", cc);
        if (system(cmd) != 0)
            fprintf(stderr, "forgectrl: iw reg set %s failed\n", cc);
    }
    if (system("iw dev wlan0 set power_save off >/dev/null 2>&1") != 0)
        fprintf(stderr, "forgectrl: wifi power_save off failed\n");
}

/* Machine identity, derived from the i.MX6 OCOTP fuses exactly like
 * the factory firmware: the serial is fuse word HW_OCOTP_MAC0 (nvmem
 * word 34 on the mainline imx-ocotp driver; hex text on the legacy
 * fsl_otp path), and the factory hostname is that serial encoded
 * base-23 over the alphabet BCDFGHJKMQRTVWXY2346789, up to six
 * characters, split XXX-YYY. The GUI always identifies the machine by
 * this fuse identity - the gf_serial setting overrides only what is
 * sent to the Glowforge cloud. */
static unsigned long fuse_serial(void)
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
static int fuse_password(char *buf, size_t len)
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

static void machine_id(char *buf, size_t len)
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

static int reply_settings(struct _u_response *res)
{
    char body[3072], val[128], mid[16], fwver[48];
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
    /* A zero flow-check window disables coolant-flow interrogation
     * entirely - a real machine state the panel must surface loudly
     * rather than present as a bare "0". */
    char fcs[128];
    int flow_off = settings_get("cool_flow_check_s", fcs, sizeof(fcs)) == 0 &&
                   strtod(fcs, NULL) == 0.0;
    append(body, sizeof(body), &off,
           "\"flow_checks_disabled\":%s,", flow_off ? "true" : "false");
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
    char body[768];
    machine_status_json(body, sizeof(body));
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
    double tmax, tresume;
    effective_temp(req, "cool_temp_max", 33.0, &tmax);
    effective_temp(req, "cool_temp_resume", 31.0, &tresume);
    if (tresume >= tmax)
        return reply_error(res, 400,
            "cool_temp_resume must be below cool_temp_max");

    for (size_t i = 0; i < N_SETTINGS; i++) {
        const char *v = setting_param(req, setting_defs[i].key);
        if (!v)
            continue;
        if (settings_set(setting_defs[i].key, v) != 0)
            return reply_error(res, 500, "cannot write settings file");
        fprintf(stderr, "forgectrl: %s %s\n", setting_defs[i].key,
                !v[0] ? "cleared" :
                setting_defs[i].secret ? "set" : v);
    }
    if (setting_param(req, "wifi_country"))
        apply_wifi(1);
    return reply_settings(res);
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

    if (cool_state_report(mode, armed, duty[0], duty[1], duty[2]) != 0)
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
    char body[512];
    cool_status_json(body, sizeof(body));
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

/* The panel with the per-machine token spliced in place of the
 * __FFTOKEN__ placeholder, built once. Serving the token inside the
 * page (rather than from an endpoint any LAN client could call) is what
 * lets the origin checks keep it out of a rebinding attacker's reach. */
static const char *panel_html(void)
{
    static char *page;
    if (page)
        return page;

    const char *mark = strstr(index_html, "__FFTOKEN__");
    const char *tok = auth_token();
    if (!mark || !tok[0])
        return index_html;              /* no token: serve inert page */

    size_t pre = (size_t)(mark - index_html);
    size_t tlen = strlen(tok);
    size_t total = strlen(index_html) - strlen("__FFTOKEN__") + tlen + 1;
    page = malloc(total);
    if (!page)
        return index_html;
    memcpy(page, index_html, pre);
    memcpy(page + pre, tok, tlen);
    strcpy(page + pre + tlen, mark + strlen("__FFTOKEN__"));
    return page;
}

/* "/" serves the UI (ui.c), plus the mjpg-streamer-compatible
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

int main(void)
{
    unsigned port = DEFAULT_PORT;
    const char *v = getenv("FORGECTRL_PORT");
    if (v && atoi(v) > 0 && atoi(v) < 65536)
        port = (unsigned)atoi(v);

    /* Stay well below the motion feeder (SCHED_FIFO) and the controller;
     * best effort. */
    (void)nice(5);

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

    struct _u_instance inst;
    if (ulfius_init_instance(&inst, port, NULL, NULL) != U_OK) {
        fprintf(stderr, "forgectrl: ulfius init failed\n");
        return 1;
    }
    ulfius_add_endpoint_by_val(&inst, "GET", "/", NULL, 0, &cb_root, NULL);
    ulfius_add_endpoint_by_val(&inst, "GET", "/cam/stream", NULL, 0,
                               &cb_stream, NULL);
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
    ulfius_set_upload_file_callback_function(&inst, &update_upload_sink,
                                             NULL);

    if (ulfius_start_framework(&inst) != U_OK) {
        fprintf(stderr, "forgectrl: cannot start HTTP on port %u\n", port);
        ulfius_clean_instance(&inst);
        return 1;
    }
    fprintf(stderr, "forgectrl: listening on port %u\n", port);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    while (!quit)
        pause();

    fprintf(stderr, "forgectrl: shutting down\n");
    ulfius_stop_framework(&inst);
    ulfius_clean_instance(&inst);
    super_shutdown();
    cool_shutdown();
    cam_engine_shutdown();
    return 0;
}

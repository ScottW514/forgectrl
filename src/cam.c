/*
 * cam.c - persistent Glowforge camera capture engine
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Pipeline model (mainline imx-media): both sensors feed one video-mux ->
 * MIPI CSI-2 -> IPU CSI path to the 'ipu1_csi0 capture' video node. Camera
 * selection is which mux sink link is enabled; sensor controls live on the
 * sensor subdev; illumination is the per-camera LED driven via sysfs. Links
 * and pad formats are configured with media-ctl / v4l2-ctl (the same
 * sequences python3-gfhardware uses for one-shot grabs), then the capture
 * node is held open and streamed continuously.
 *
 * Two sensors ship in the field - the 5 MP OV5648 and, in "HD" machines, the
 * 8 MP OV8856 - and they differ in geometry, bit depth and control set, so
 * everything downstream of the media graph is driven from the sensor profile
 * that matches whichever driver bound (see sensor_profiles).
 *
 * PRIVACY GATE: neither camera captures unless the lid is closed. An open
 * lid points the lid camera into the room, and in cloud mode the image
 * request comes from a remote service, so the check lives here - at the one
 * process that owns the capture path - rather than at each caller: the
 * engine refuses to start, and a lid that opens mid-capture stops it. The
 * check fails closed (see machine_lid_closed()).
 *
 * Threading: a control mutex serializes engine start/stop/switch; the
 * engine lock covers frame data and counters. The worker thread only ever
 * takes the engine lock, so control paths may join it while holding the
 * control mutex.
 */
#define _GNU_SOURCE
#include "cam.h"
#include "camhealth.h"
#include "debayer.h"
#include "fflog.h"
#include "gpu_debayer.h"
#include "ipu_copy.h"
#include "mp4mux.h"
#include "settings.h"
#include "status.h"
#include "vpu_h264.h"
#include "vpu_jpeg.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>    /* requires stdio.h first (FILE) */
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define HFLIP  1          /* factory image orientation (see debayer.h) */

#define N_BUFS          4
#define DQ_TIMEOUT_S    2   /* select() timeout per frame; also the idle tick */
#define MAX_DQ_TIMEOUTS 3   /* consecutive timeouts -> engine gives up */
#define IDLE_STOP_S     10  /* no clients/snapshots for this long -> teardown */
#define LAMP_SKIP_FRAMES 3  /* frames discarded after a lamp change (in-flight
                             * exposures predate the new scene lighting) */
#define SNAP_TIMEOUT_S  15
#define CLIENT_WAIT_S   5
#define SWITCH_GRACE_S  3   /* wait this long for clients to drain on switch */

#define CAPTURE_ENTITY "ipu1_csi0 capture"

struct camdef {
    const char *name;
    int         bus;       /* I2C bus: sensor entity resolved by <bus>-0036 */
    int         muxpad;    /* video-mux sink pad */
    const char *lamp;      /* sysfs illumination attribute */
};

static const struct camdef camdefs[2] = {
    [CAM_LID]  = { "lid",  0, 0, "/sys/glowforge/pic/lid_led"    },
    [CAM_HEAD] = { "head", 3, 1, "/sys/glowforge/head/white_led" },
};

/* ------------------------------------------------------ sensor profiles */

/* Everything that differs between the sensors the machine ships with. The
 * media entity name carries the driver that bound ("ov5648 0-0036" /
 * "ov8856 0-0036"), so the profile follows the hardware without a build-time
 * switch: one image covers both. */
struct sensor_profile {
    const char *driver;     /* media entity name prefix */
    const char *model;      /* reported by /cam/status */
    int         w, h;       /* raw frame the pipeline is configured for */
    int         fps;        /* the mode's sensor frame rate */
    const char *mbus;       /* media-ctl pad format, geometry appended */
    uint32_t    pixfmt;     /* V4L2 pixel format at the capture node */
    int         bpp;        /* bytes per raw sample at the capture node */
    int         shift;      /* right shift from sample to 8-bit value */
    /* Manual exposure/gain/white balance on the sensor subdev. */
    int       (*ctrls)(const char *subdev, cam_id_t cam);
};

static int ctrls_ov5648(const char *subdev, cam_id_t cam);
static int ctrls_ov8856(const char *subdev, cam_id_t cam);

static const struct sensor_profile sensor_profiles[] = {
    {
        /* 5 MP, 8-bit Bayer straight out of the CSI. */
        .driver = "ov5648", .model = "OV5648",
        .w = 2592, .h = 1944, .fps = 15,
        .mbus = "SBGGR8_1X8", .pixfmt = V4L2_PIX_FMT_SBGGR8,
        .bpp = 1, .shift = 0, .ctrls = ctrls_ov5648,
    },
    {
        /* 8 MP "HD" modules, full frame. The sensor's RAW10 full-resolution
         * 2-lane mode asks for 1.44 Gbps/lane and the i.MX6 CSI-2 D-PHY
         * stops at 1 Gbps, but its RAW8 one carries the same frame at half
         * that - so the capture word, the Bayer order and the whole
         * downstream path are the OV5648's, only larger.
         * UNPROVEN: no 8 MP machine has been on the bench. */
        .driver = "ov8856", .model = "OV8856",
        .w = 3264, .h = 2448, .fps = 15,
        .mbus = "SBGGR8_1X8", .pixfmt = V4L2_PIX_FMT_SBGGR8,
        .bpp = 1, .shift = 0, .ctrls = ctrls_ov8856,
    },
};

#define N_PROFILES ((int)(sizeof(sensor_profiles) / sizeof(sensor_profiles[0])))

/* Worst case over every profile, so the worker's scratch buffers are sized
 * once and survive a snapshot borrow of a differently-modeled camera. */
static size_t max_raw8_bytes(void)
{
    size_t m = 0;
    for (int i = 0; i < N_PROFILES; i++) {
        size_t n = (size_t)sensor_profiles[i].w * sensor_profiles[i].h;
        if (n > m)
            m = n;
    }
    return m;
}

static size_t max_half_rgb_bytes(void)
{
    size_t m = 0;
    for (int i = 0; i < N_PROFILES; i++) {
        size_t n = (size_t)(sensor_profiles[i].w / 2) *
                   (sensor_profiles[i].h / 2) * 3;
        if (n > m)
            m = n;
    }
    return m;
}

/* "ov8856 3-0036" -> the OV8856 profile. NULL for a driver we have no
 * profile for (the engine then refuses to start rather than guess). */
static const struct sensor_profile *profile_for(const char *entity)
{
    for (int i = 0; i < N_PROFILES; i++) {
        size_t n = strlen(sensor_profiles[i].driver);
        if (!strncmp(entity, sensor_profiles[i].driver, n) &&
            (entity[n] == ' ' || entity[n] == '\0'))
            return &sensor_profiles[i];
    }
    return NULL;
}

struct buffer {
    void  *start;
    size_t length;
    int    dmabuf;      /* exported for the GPU import; -1 until then */
};

static struct {
    /* control path (start/stop/switch) - taken before lock, never by the
     * worker thread */
    pthread_mutex_t ctl;

    /* engine state */
    pthread_mutex_t lock;
    pthread_cond_t  frame_cv;   /* new stream frame published */
    pthread_cond_t  snap_cv;    /* snapshot request completed */
    pthread_t       tid;
    int             tid_valid;
    int             running;    /* worker alive and capturing */
    int             stop_flag;
    cam_id_t        cam;        /* camera the pipeline is configured for
                                 * RIGHT NOW (a borrow flips it briefly) */
    cam_id_t        home_cam;   /* camera the engine serves for streaming -
                                 * what arbitration must compare against */
    /* Sensor that bound on each camera's I2C bus, last time one was
     * resolved; NULL until then. `prof` is the profile the pipeline is
     * configured for RIGHT NOW (it follows eng.cam through a borrow). */
    const struct sensor_profile *seen[2];
    const struct sensor_profile *prof;
    int             clients;
    uint64_t        kick_gen;   /* bumped to preempt all current stream
                                 * clients (their streams end cleanly) */
    struct timespec last_activity;

    /* published stream frame (half-res JPEG) */
    uint8_t        *stream_jpg;
    size_t          stream_len;
    size_t          stream_cap;
    uint64_t        seq;
    double          fps;

    /* published H.264 access unit (worker writes, H.264 clients read) */
    pthread_cond_t  h264_cv;
    uint8_t        *h264_au;
    size_t          h264_len;
    uint64_t        h264_seq;
    uint64_t        h264_pts;   /* 90 kHz, CLOCK_MONOTONIC based */
    int             h264_key;
    int             h264_clients;
    int             h264_up;        /* encoder open and delivering */
    int             h264_key_req;   /* a joining client needs an IDR */

    /* one pending snapshot request at a time (control mutex serializes).
     * snap_cam may differ from the streaming camera: the worker then
     * "borrows" the mux - pauses the stream, switches, grabs one frame,
     * switches back (stream clients see a few-second freeze). */
    int             snap_pending;   /* 1 = requested, 2 = done, 3 = failed */
    cam_id_t        snap_cam;
    int             snap_full;
    int             snap_quality;
    int             snap_lamp;      /* per-shot lamp override, -1 = default */
    uint8_t        *snap_jpg;       /* malloc'd result, taken by requester */
    size_t          snap_len;

    /* capture resources (worker/start/teardown only) */
    int             fd;
    struct buffer   bufs[N_BUFS];
    int             n_bufs;
    int             streaming;
    int             lid_stopped;    /* the worker exited on an open lid;
                                     * cleared when an engine start
                                     * succeeds (which needs a closed lid) */
    int             lamp_prev;      /* -1 = unknown, restore to 0 */
    int             cached_bufs;    /* capture mmaps are CPU-cached
                                     * (non-coherent); no bounce copy */
    /* Frame-health ladder (camhealth.h). Like the capture resources above
     * it is written before the worker exists or by the worker itself. */
    struct cam_health health;

    /* Frame health totals, for /cam/status and the logs. Cumulative since
     * the daemon started, so a machine with a marginal camera cable shows
     * up as a nonzero corrupt count days later. */
    uint64_t        frames;         /* dequeued */
    uint64_t        corrupt;        /* of those, flagged errored */
    unsigned        recoveries;     /* stream restarts */

    /* config */
    int             stream_quality;
    int             lamp_level;
    double          fps_cap;        /* stream frames/s ceiling; 0 = sensor max */
    int             h264_kbps;
    int             h264_gop;
    int             vpu_active;     /* last stream frame went through the VPU */
    int             gpu_active;     /* last stream frame demosaiced on the GPU */
    int             hw_skip;        /* CSI frame skip realizes the fps cap */
} eng = {
    .ctl = PTHREAD_MUTEX_INITIALIZER,
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .frame_cv = PTHREAD_COND_INITIALIZER,
    .snap_cv = PTHREAD_COND_INITIALIZER,
    .h264_cv = PTHREAD_COND_INITIALIZER,
    .fd = -1,
    .lamp_prev = -1,
    .stream_quality = 75,
    .lamp_level = 132,
    .h264_kbps = 1500,
    .h264_gop = 30,
};

const char *cam_name(cam_id_t cam)
{
    return camdefs[cam].name;
}


/* CODA960 hardware JPEG encoder for the stream path; libjpeg remains the
 * fallback (and the snapshot path). Worker-thread use only. */
static vpu_jpeg_t *vpu;
static int vpu_disabled;

/* CODA960 H.264 encoder (the BIT processor, independent of the JPEG
 * unit), plus the engine-held parameter-set cache viewers that join
 * late take their SPS/PPS from. Worker-thread use only. */
static vpu_h264_t *h264;
static int h264_disabled;
static mp4mux_t *h264_params;
static pthread_mutex_t h264_params_mx = PTHREAD_MUTEX_INITIALIZER;

/* GC880 GPU demosaic for the stream path; the NEON path remains the
 * fallback (and always the snapshot path). The GPU renders into the IPU
 * stride-fix buffer and the IPU crops that into each encoder (the GPU's
 * row padding and the CODA's fixed stride are incompatible; ipu_copy.h).
 * Worker-thread use only. */
static gpu_debayer_t *gpu;
static ipu_copy_t *ipu;
static int gpu_disabled;
static int hw_skip_disabled;

/* The GPU pipeline's in-flight frame (worker thread only): the IPU
 * source slot being rendered, the capture buffer held out of the queue
 * for it, and its timestamp. Reset wherever the GPU is torn down - the
 * buffers it refers to go back to the queue through STREAMOFF or the
 * teardown paths, never through a stale delivery. */
static int pend_slot = -1;
static unsigned pend_idx;
static uint64_t pend_pts;

/* FORGECTRL_NO_CACHED_BUFS: never request non-coherent capture buffers
 * (forces the uncached-mmap + bounce-copy path). */
static int cached_disabled;

/* ------------------------------------------------------------------ util */

static void now_ts(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

static double ts_diff(const struct timespec *a, const struct timespec *b)
{
    return (double)(a->tv_sec - b->tv_sec) +
           (double)(a->tv_nsec - b->tv_nsec) / 1e9;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* Run a shell command, logging and returning nonzero on failure. */
static int run(const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    int rc = system(cmd);
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0) {
        fflog(LOG_ERR, "cam: command failed (%d): %s", rc, cmd);
        return -1;
    }
    return 0;
}

/* Run a command and capture its first line of output. */
static int run_read(char *out, size_t outlen, const char *fmt, ...)
{
    char cmd[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    out[0] = '\0';
    if (!fgets(out, (int)outlen, p)) {
        pclose(p);
        return -1;
    }
    int rc = pclose(p);
    out[strcspn(out, "\r\n")] = '\0';
    if (rc == -1 || !WIFEXITED(rc) || WEXITSTATUS(rc) != 0 || !out[0]) {
        fflog(LOG_ERR, "cam: command failed: %s", cmd);
        return -1;
    }
    return 0;
}

static int sysfs_read_int(const char *path, int *val)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fscanf(f, "%d", val) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static int sysfs_write_int(const char *path, int val)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    int ok = fprintf(f, "%d", val) > 0;
    fclose(f);
    return ok ? 0 : -1;
}

/* The lid lamp at idle. The PIC lights it at power-on; a warm reboot
 * leaves it dark (the module's remove path turns it off) and the cloud
 * client sets its own level, so the daemon asserts the configured idle
 * level itself: at start, on a settings change, and whenever the
 * supervisor spawns a controller. Setting lid_lamp_idle, 0-255. */
#define LAMP_IDLE_DEFAULT 236

int cam_lamp_idle_level(void)
{
    char v[16];
    if (settings_get("lid_lamp_idle", v, sizeof(v)) == 0 && v[0]) {
        char *end;
        long l = strtol(v, &end, 10);
        if (end != v && *end == '\0' && l >= 0 && l <= 255)
            return (int)l;
    }
    return LAMP_IDLE_DEFAULT;
}

void cam_lamp_apply_idle(void)
{
    int level = cam_lamp_idle_level();
    pthread_mutex_lock(&eng.ctl);
    pthread_mutex_lock(&eng.lock);
    int lid_capturing = eng.running && eng.cam == CAM_LID && eng.lamp_prev >= 0;
    pthread_mutex_unlock(&eng.lock);
    if (lid_capturing)
        eng.lamp_prev = level;      /* restored at the capture's teardown */
    else
        sysfs_write_int(camdefs[CAM_LID].lamp, level);
    pthread_mutex_unlock(&eng.ctl);
    fflog(LOG_INFO, "cam: lid lamp idle level %d%s", level,
          lid_capturing ? " (applies after the capture)" : "");
}

/* --------------------------------------------------- pipeline configure */

/* Resolve the sensor media entity on an I2C bus (e.g. "ov5648 0-0036") by
 * its address suffix, so OV5648 and OV8856 (HD model) both match. */
static int sensor_entity(int bus, char *out, size_t outlen)
{
    char needle[16];
    snprintf(needle, sizeof(needle), " %d-0036", bus);

    FILE *p = popen("media-ctl -p", "r");
    if (!p)
        return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), p)) {
        char *e = strstr(line, "entity ");
        if (!e)
            continue;
        char *colon = strchr(e, ':');
        char *hit = strstr(line, needle);
        if (!colon || !hit || hit < colon)
            continue;
        char *start = colon + 2;
        char *end = hit + strlen(needle);
        if (end <= start || (size_t)(end - start) >= outlen)
            continue;
        memcpy(out, start, (size_t)(end - start));
        out[end - start] = '\0';
        found = 1;
        break;
    }
    pclose(p);
    return found ? 0 : -1;
}

/* Route the selected sensor through the video-mux to the capture node and
 * set the raw-Bayer format on every pad of the active path. Exactly one
 * mux sink link may be enabled, so the other camera's link (if that sensor
 * exists) is disabled first. */
static int configure_pipeline(cam_id_t cam, const char *sensor,
                              const char *other_sensor,
                              const struct sensor_profile *p)
{
    const struct camdef *c = &camdefs[cam];
    const struct camdef *o = &camdefs[cam == CAM_LID ? CAM_HEAD : CAM_LID];
    char fmt[64];

    /* 'field:none' is mandatory - without it link validation rejects
     * STREAMON with -EPIPE. */
    snprintf(fmt, sizeof(fmt), "%s/%dx%d field:none", p->mbus, p->w, p->h);

    if (other_sensor[0] &&
        run("media-ctl -l '\"%s\":0 -> \"video-mux\":%d [0]'",
            other_sensor, o->muxpad))
        return -1;

    if (run("media-ctl -l '\"%s\":0 -> \"video-mux\":%d [1]'",
            sensor, c->muxpad) ||
        run("media-ctl -l '\"video-mux\":2 -> \"imx6-mipi-csi2\":0 [1]'") ||
        run("media-ctl -l '\"imx6-mipi-csi2\":1 -> \"ipu1_csi0_mux\":0 [1]'") ||
        run("media-ctl -l '\"ipu1_csi0_mux\":5 -> \"ipu1_csi0\":0 [1]'") ||
        run("media-ctl -l '\"ipu1_csi0\":2 -> \"" CAPTURE_ENTITY "\":0 [1]'"))
        return -1;

    if (run("media-ctl -V '\"%s\":0 [fmt:%s]'", sensor, fmt) ||
        run("media-ctl -V '\"video-mux\":%d [fmt:%s]'", c->muxpad, fmt) ||
        run("media-ctl -V '\"video-mux\":2 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"imx6-mipi-csi2\":0 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"imx6-mipi-csi2\":1 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0_mux\":0 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0_mux\":5 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0\":0 [fmt:%s]'", fmt) ||
        run("media-ctl -V '\"ipu1_csi0\":2 [fmt:%s]'", fmt))
        return -1;

    /* An fps cap of at least 1 is realized in hardware where possible:
     * the IPU CSI's frame-skip table drops frames before they are ever
     * DMA-written, so a skipped frame costs no memory bandwidth and no
     * CPU (the software pacing in the worker still runs and passes what
     * arrives). The sink pad carries the sensor rate, the IDMAC source
     * pad the requested one; the driver picks the nearest skip pattern.
     * Best effort: a media-ctl without interval syntax leaves the cap to
     * software pacing alone. */
    int hw = 0;
    if (!hw_skip_disabled && eng.fps_cap >= 1.0 &&
        eng.fps_cap < (double)p->fps) {
        int n = (int)(eng.fps_cap + 0.5);
        char fmt_in[80], fmt_out[80];
        snprintf(fmt_in, sizeof(fmt_in), "%s/%dx%d@1/%d field:none",
                 p->mbus, p->w, p->h, p->fps);
        snprintf(fmt_out, sizeof(fmt_out), "%s/%dx%d@1/%d field:none",
                 p->mbus, p->w, p->h, n);
        if (run("media-ctl -V '\"ipu1_csi0\":0 [fmt:%s]'", fmt_in) == 0 &&
            run("media-ctl -V '\"ipu1_csi0\":2 [fmt:%s]'", fmt_out) == 0) {
            hw = 1;
            fflog(LOG_INFO, "cam: CSI hardware frame skip to ~%d fps", n);
        } else {
            fflog(LOG_WARNING, "cam: CSI frame-skip setup refused, the "
                  "fps cap stays software-paced");
        }
    }
    pthread_mutex_lock(&eng.lock);
    eng.hw_skip = hw;
    pthread_mutex_unlock(&eng.lock);
    return 0;
}

/* Manual exposure/gain/white-balance on the sensor subdev (factory values).
 * The auto-clusters must go manual before the manual values take effect.
 * The sensor flips stay off: HFLIP breaks imx-media CSI capture, so the
 * factory mirror is applied in software (debayer).
 *
 * Exposure is in 1/16-line units and cannot exceed the frame length: the
 * 2592x1944 mode is 1984 lines, so the usable ceiling is ~31600. Gain is in
 * 1/16 steps (16 = 1x). */
static int ctrls_ov5648(const char *subdev, cam_id_t cam)
{
    static const struct { int exposure, gain; } d[2] = {
        [CAM_LID]  = { 24000,  50 },
        [CAM_HEAD] = { 24000, 200 },
    };

    if (run("v4l2-ctl -d %s -c auto_exposure=1 -c gain_automatic=0"
            " -c white_balance_automatic=0", subdev))
        return -1;
    if (run("v4l2-ctl -d %s -c exposure=%d -c gain=%d -c red_balance=1100"
            " -c blue_balance=1400 -c horizontal_flip=0 -c vertical_flip=0",
            subdev, d[cam].exposure, d[cam].gain))
        return -1;
    return 0;
}

/* The OV8856 driver exposes a different set: exposure counts whole lines
 * (it shifts into the 1/16-line register itself) and is capped by the frame
 * length - 2482 lines in the 3264x2448 mode - analogue gain is 128 = 1x,
 * and there are no auto-exposure, auto-gain or white-balance controls to
 * switch off, so the sensor comes up manual. The flips are still forced off
 * for the same reason as the OV5648.
 *
 * UNPROVEN: these are the OV5648 defaults translated into the OV8856's
 * units - the same fraction of the frame (76%) and the same gain multiple
 * (3.1x lid, 12.5x head). They are a starting point for commissioning on a
 * real 8 MP machine, not measured values, and the driver publishes no
 * red/blue balance controls at all, so white balance is uncorrected. */
static int ctrls_ov8856(const char *subdev, cam_id_t cam)
{
    static const struct { int exposure, gain; } d[2] = {
        [CAM_LID]  = { 1886,  400 },
        [CAM_HEAD] = { 1886, 1600 },
    };

    if (run("v4l2-ctl -d %s -c exposure=%d -c analogue_gain=%d"
            " -c digital_gain=1024 -c horizontal_flip=0 -c vertical_flip=0",
            subdev, d[cam].exposure, d[cam].gain))
        return -1;
    return 0;
}

static int configure_sensor(const char *sensor, const struct sensor_profile *p,
                            cam_id_t cam)
{
    char subdev[64];
    if (run_read(subdev, sizeof(subdev), "media-ctl -e '%s'", sensor))
        return -1;
    return p->ctrls(subdev, cam);
}

/* ------------------------------------------------------- jpeg encoding */

static int jpeg_encode_rgb(const uint8_t *rgb, int w, int h, int quality,
                           int fast, uint8_t **out, size_t *outlen)
{
    struct jpeg_compress_struct ci;
    struct jpeg_error_mgr jerr;
    unsigned char *buf = NULL;
    unsigned long buflen = 0;

    ci.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&ci);
    jpeg_mem_dest(&ci, &buf, &buflen);
    ci.image_width = (JDIMENSION)w;
    ci.image_height = (JDIMENSION)h;
    ci.input_components = 3;
    ci.in_color_space = JCS_RGB;
    jpeg_set_defaults(&ci);
    jpeg_set_quality(&ci, quality, TRUE);
    if (fast)
        ci.dct_method = JDCT_FASTEST;
    jpeg_start_compress(&ci, TRUE);
    while (ci.next_scanline < ci.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (long)ci.next_scanline * w * 3);
        jpeg_write_scanlines(&ci, &row, 1);
    }
    jpeg_finish_compress(&ci);
    jpeg_destroy_compress(&ci);
    *out = buf;
    *outlen = (size_t)buflen;
    return 0;
}

/* --------------------------------------------------- capture start/stop */

/* Release every capture resource and restore the lamp. Safe to call from
 * any state; called by the worker on exit and by a failed start. */
static void release_capture(void)
{
    /* The GPU holds imports of the capture buffers about to go away;
     * it re-imports on the next stream frame (the encoders and their
     * dmabufs survive, so only the raw side is redone). */
    if (gpu) {
        gpu_debayer_close(gpu);
        gpu = NULL;
    }
    pend_slot = -1;
    if (eng.streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(eng.fd, VIDIOC_STREAMOFF, &type);
        eng.streaming = 0;
    }
    for (int i = 0; i < eng.n_bufs; i++) {
        if (eng.bufs[i].start) {
            munmap(eng.bufs[i].start, eng.bufs[i].length);
            eng.bufs[i].start = NULL;
        }
        if (eng.bufs[i].dmabuf >= 0) {
            close(eng.bufs[i].dmabuf);
            eng.bufs[i].dmabuf = -1;
        }
    }
    eng.n_bufs = 0;
    if (eng.fd >= 0) {
        close(eng.fd);
        eng.fd = -1;
    }
    if (eng.lamp_prev >= 0) {
        sysfs_write_int(camdefs[eng.cam].lamp, eng.lamp_prev);
        eng.lamp_prev = -1;
    }
}

/* Stop and restart the capture queue on the open node, leaving the media
 * graph, the sensor and the lamp alone. This is the recovery for a sensor
 * that has lost CSI-2 sync: the frames it produces come back flagged
 * errored until the receiver is re-synchronized, and cycling the queue is
 * what re-synchronizes it. Worker thread only. */
static int restream(void)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(eng.fd, VIDIOC_STREAMOFF, &type) < 0)
        return -1;
    eng.streaming = 0;
    /* STREAMOFF returns every buffer to the dequeued state. */
    for (int i = 0; i < eng.n_bufs; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned)i;
        if (xioctl(eng.fd, VIDIOC_QBUF, &buf) < 0)
            return -1;
    }
    if (xioctl(eng.fd, VIDIOC_STREAMON, &type) < 0)
        return -1;
    eng.streaming = 1;
    cam_health_restarted(&eng.health);
    pthread_mutex_lock(&eng.lock);
    eng.recoveries++;
    pthread_mutex_unlock(&eng.lock);
    return 0;
}

/* Configure the media graph and sensor, light the lamp, and bring up the
 * V4L2 capture node streaming. Called with ctl held, engine not running. */
static int start_capture(cam_id_t cam, char *err, size_t errlen)
{
    const struct camdef *c = &camdefs[cam];
    char sensor[64], other[64] = "";

    /* Privacy gate, checked before the media graph is touched and before
     * the lamp is raised, so an open lid leaves no trace of an attempt. */
    if (!machine_lid_closed()) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return -1;
    }

    if (sensor_entity(c->bus, sensor, sizeof(sensor))) {
        snprintf(err, errlen, "no camera sensor on i2c-%d", c->bus);
        return -1;
    }
    const struct sensor_profile *p = profile_for(sensor);
    if (!p) {
        snprintf(err, errlen, "unsupported camera sensor '%s'", sensor);
        return -1;
    }
    (void)sensor_entity(camdefs[cam == CAM_LID ? CAM_HEAD : CAM_LID].bus,
                        other, sizeof(other));

    if (configure_pipeline(cam, sensor, other, p)) {
        snprintf(err, errlen, "media pipeline configuration failed");
        return -1;
    }
    if (configure_sensor(sensor, p, cam)) {
        snprintf(err, errlen, "sensor configuration failed");
        return -1;
    }

    char dev[64];
    if (run_read(dev, sizeof(dev), "media-ctl -e '" CAPTURE_ENTITY "'")) {
        snprintf(err, errlen, "cannot resolve capture video node");
        return -1;
    }

    pthread_mutex_lock(&eng.lock);
    eng.cam = cam;
    eng.prof = p;
    eng.seen[cam] = p;
    eng.lid_stopped = 0;    /* reaching here means the lid read closed */
    pthread_mutex_unlock(&eng.lock);

    /* Scene lighting for the duration; the previous level is restored at
     * teardown (raw register write - instant, no fade). */
    if (sysfs_read_int(c->lamp, &eng.lamp_prev))
        eng.lamp_prev = 0;
    sysfs_write_int(c->lamp, eng.lamp_level);

    /* O_CLOEXEC: a controller spawned while the capture is up must not
     * inherit this descriptor - the buffers stay allocated for as long as
     * any copy is open, and every later S_FMT fails with EBUSY. */
    eng.fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
    if (eng.fd < 0) {
        snprintf(err, errlen, "open %s: %s", dev, strerror(errno));
        release_capture();
        return -1;
    }

    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (unsigned)p->w;
    fmt.fmt.pix.height = (unsigned)p->h;
    fmt.fmt.pix.pixelformat = p->pixfmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(eng.fd, VIDIOC_S_FMT, &fmt) < 0) {
        snprintf(err, errlen, "S_FMT: %s", strerror(errno));
        release_capture();
        return -1;
    }
    if ((int)fmt.fmt.pix.width != p->w || (int)fmt.fmt.pix.height != p->h ||
        fmt.fmt.pix.pixelformat != p->pixfmt) {
        snprintf(err, errlen, "capture node gave %ux%u fourcc %.4s, "
                 "not %dx%d for %s", fmt.fmt.pix.width, fmt.fmt.pix.height,
                 (const char *)&fmt.fmt.pix.pixelformat, p->w, p->h,
                 p->model);
        release_capture();
        return -1;
    }

    struct v4l2_requestbuffers req = {0};
    req.count = N_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    /* Ask for non-coherent = CPU-cached mappings: the CSI DMA-writes the
     * frames either way, but a cached mapping lets the demosaic read them
     * at cached speed (vb2 invalidates the CPU cache during DQBUF). A
     * capture queue without cache-hint support ignores the flag and omits
     * the MMAP_CACHE_HINTS capability; the bounce-copy path covers that. */
    if (!cached_disabled)
        req.flags = V4L2_MEMORY_FLAG_NON_COHERENT;
    if (xioctl(eng.fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        snprintf(err, errlen, "REQBUFS: %s (device busy?)", strerror(errno));
        release_capture();
        return -1;
    }
    int cached = !cached_disabled &&
        (req.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP_CACHE_HINTS) != 0;
    pthread_mutex_lock(&eng.lock);
    eng.cached_bufs = cached;
    pthread_mutex_unlock(&eng.lock);
    fflog(LOG_INFO, "cam: %s on %s, %dx%d %s, capture buffers %s",
          p->model, c->name, p->w, p->h, p->mbus,
          cached ? "cached (non-coherent)" : "uncached (bounce copy)");

    for (eng.n_bufs = 0; eng.n_bufs < (int)req.count; eng.n_bufs++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned)eng.n_bufs;
        if (xioctl(eng.fd, VIDIOC_QUERYBUF, &buf) < 0) {
            snprintf(err, errlen, "QUERYBUF: %s", strerror(errno));
            release_capture();
            return -1;
        }
        eng.bufs[eng.n_bufs].length = buf.length;
        eng.bufs[eng.n_bufs].dmabuf = -1;
        eng.bufs[eng.n_bufs].start = mmap(NULL, buf.length,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          eng.fd, buf.m.offset);
        if (eng.bufs[eng.n_bufs].start == MAP_FAILED) {
            eng.bufs[eng.n_bufs].start = NULL;
            snprintf(err, errlen, "mmap: %s", strerror(errno));
            release_capture();
            return -1;
        }
    }

    for (int i = 0; i < eng.n_bufs; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned)i;
        if (xioctl(eng.fd, VIDIOC_QBUF, &buf) < 0) {
            snprintf(err, errlen, "QBUF: %s", strerror(errno));
            release_capture();
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(eng.fd, VIDIOC_STREAMON, &type) < 0) {
        snprintf(err, errlen, "STREAMON: %s", strerror(errno));
        release_capture();
        return -1;
    }
    eng.streaming = 1;
    cam_health_started(&eng.health);
    return 0;
}

/* ---------------------------------------------------------- worker loop */

/* Encode the pending snapshot request from a raw frame and deliver the
 * result (success or failure) to the waiter. `raw` is 8-bit BGGR at the
 * geometry of the profile the pipeline is running (eng.prof). */
static void deliver_snap(const uint8_t *raw, uint8_t *rgb_half,
                         uint8_t **prgb_full, size_t *prgb_full_cap)
{
    uint8_t *jpg = NULL;
    size_t len = 0;
    int ok;

    pthread_mutex_lock(&eng.lock);
    int full = eng.snap_full;
    int q = eng.snap_quality;
    const struct sensor_profile *p = eng.prof;
    pthread_mutex_unlock(&eng.lock);

    const int w = p->w, h = p->h;

    if (full) {
        size_t need = (size_t)w * h * 3;
        if (*prgb_full_cap < need) {
            free(*prgb_full);
            *prgb_full = malloc(need);
            *prgb_full_cap = *prgb_full ? need : 0;
        }
        ok = *prgb_full != NULL;
        if (ok) {
            debayer_bggr_bilinear(raw, *prgb_full, w, h, HFLIP);
            ok = jpeg_encode_rgb(*prgb_full, w, h, q, 0, &jpg, &len) == 0;
        }
    } else {
        debayer_bggr_half(raw, rgb_half, w, h, HFLIP);
        ok = jpeg_encode_rgb(rgb_half, w / 2, h / 2, q, 0, &jpg, &len) == 0;
    }

    pthread_mutex_lock(&eng.lock);
    free(eng.snap_jpg);
    eng.snap_jpg = ok ? jpg : NULL;
    eng.snap_len = ok ? len : 0;
    eng.snap_pending = ok ? 2 : 3;
    now_ts(&eng.last_activity);
    pthread_cond_broadcast(&eng.snap_cv);
    pthread_mutex_unlock(&eng.lock);
}

/* Mark a pending snapshot failed (only if not already delivered). */
static void fail_snap(void)
{
    pthread_mutex_lock(&eng.lock);
    if (eng.snap_pending == 1) {
        eng.snap_pending = 3;
        pthread_cond_broadcast(&eng.snap_cv);
    }
    pthread_mutex_unlock(&eng.lock);
}

/* Present a dequeued capture buffer as the 8-bit BGGR frame every demosaic
 * takes. An 8-bit sensor on cached buffers needs nothing (the mapping is
 * read directly); an 8-bit sensor on uncached buffers is bulk-copied,
 * because byte reads from a coherent mapping each cost a bus transaction
 * (demosaicing in place measures ~340 ms/frame); a 10-bit sensor is
 * narrowed into `scratch`, which is also the copy. */
static const uint8_t *prepare_raw8(const void *cap, uint8_t *scratch,
                                   const struct sensor_profile *p)
{
    size_t n = (size_t)p->w * p->h;

    if (p->bpp == 1) {
        if (eng.cached_bufs)
            return (const uint8_t *)cap;
        memcpy(scratch, cap, n);
        return scratch;
    }

    uint16_t peak = debayer_narrow16(cap, scratch, n, p->shift);
    /* One line per engine start: on a sensor whose sample alignment has
     * not been measured, the peak says whether the shift is right (a lit
     * 10-bit frame peaks near 1023, not near 65535 or near 255). */
    static const struct sensor_profile *logged;
    if (logged != p) {
        logged = p;
        fflog(LOG_DEBUG, "cam: %s raw peak sample %u (>>%d)",
              p->model, peak, p->shift);
    }
    return scratch;
}

/* Count a dequeued frame and say what to do with it (camhealth.h). */
static cam_frame_action_t classify(const struct v4l2_buffer *buf)
{
    int errored = (buf->flags & V4L2_BUF_FLAG_ERROR) != 0;

    pthread_mutex_lock(&eng.lock);
    eng.frames++;
    if (errored)
        eng.corrupt++;
    pthread_mutex_unlock(&eng.lock);

    return cam_health_frame(&eng.health, errored);
}

/* Capture one frame from the currently-started pipeline and feed it to
 * deliver_snap. Used by the borrow path. The first `skip` dequeued frames
 * are requeued unused (frames already in flight predate a just-changed lamp
 * level), as are the stream's warm-up frames and any frame the queue flags
 * errored. A borrow that cannot get a clean frame fails the snapshot rather
 * than restarting anything: the caller tears the borrowed pipeline down
 * either way. */
static int grab_one_snap(uint8_t *raw_cached, uint8_t *rgb_half,
                         uint8_t **prgb_full, size_t *prgb_full_cap,
                         int skip)
{
    /* Enough iterations for the lamp drain, the warm-up drain and the bad
     * frames the ladder tolerates, plus MAX_DQ_TIMEOUTS empty waits. */
    int budget = skip + CAM_WARMUP_FRAMES + CAM_MAX_BAD_FRAMES +
                 MAX_DQ_TIMEOUTS;

    for (int tries = 0; tries < MAX_DQ_TIMEOUTS && budget-- > 0; tries++) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(eng.fd, &fds);
        struct timeval tv = { .tv_sec = DQ_TIMEOUT_S };
        int r = select(eng.fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1 && errno == EINTR) {
            tries--;
            continue;
        }
        if (r <= 0)
            continue;
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(eng.fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EIO)
                continue;
            return -1;
        }
        cam_frame_action_t act = classify(&buf);
        if (act == CAM_FRAME_RESTART || act == CAM_FRAME_ABORT) {
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            return -1;
        }
        if (act != CAM_FRAME_USE) {     /* WARMUP or DROP */
            tries--;
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            continue;
        }
        if (skip > 0) {
            skip--;
            tries--;
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            continue;
        }
        deliver_snap(prepare_raw8(eng.bufs[buf.index].start, raw_cached,
                                  eng.prof),
                     rgb_half, prgb_full, prgb_full_cap);
        xioctl(eng.fd, VIDIOC_QBUF, &buf);
        return 0;
    }
    return -1;
}

/* ---------------------------------------------- stream encoder plumbing */

/* Geometry the encoder set (worker thread only). */
static int enc_w, enc_h;

/* Open (or re-open, on a geometry change) whichever encoders the current
 * clients need, and the GPU demosaic that feeds them. Every piece fails
 * soft: a missing encoder or GPU disables that path and the frame loop
 * uses what came up. */
static void ensure_encoders(int jpeg_want, int h264_want,
                            int half_w, int half_h, int hflip)
{
    if (enc_w != half_w || enc_h != half_h) {
        if (vpu) {
            vpu_jpeg_close(vpu);
            vpu = NULL;
        }
        if (h264) {
            vpu_h264_close(h264);
            h264 = NULL;
        }
        if (gpu) {
            gpu_debayer_close(gpu);
            gpu = NULL;
        }
        if (ipu) {
            ipu_copy_close(ipu);
            ipu = NULL;
        }
        pend_slot = -1;
        pthread_mutex_lock(&h264_params_mx);
        mp4mux_free(h264_params);
        h264_params = NULL;
        pthread_mutex_unlock(&h264_params_mx);
        enc_w = half_w;
        enc_h = half_h;
    }

    if (jpeg_want && !vpu_disabled && !vpu) {
        vpu = vpu_jpeg_open(half_w, half_h, eng.stream_quality);
        if (!vpu) {
            vpu_disabled = 1;
            fflog(LOG_WARNING, "cam: no VPU JPEG encoder, "
                  "using software encode");
        }
    }
    if (h264_want && !h264_disabled && !h264) {
        int fps = eng.fps_cap >= 1.0 ? (int)(eng.fps_cap + 0.5)
                                     : eng.prof->fps;
        h264 = vpu_h264_open(half_w, half_h, fps, eng.h264_kbps * 1000,
                             eng.h264_gop);
        if (!h264) {
            h264_disabled = 1;
            fflog(LOG_WARNING, "cam: no H.264 encoder, the stream "
                  "stays MJPEG only");
        } else {
            pthread_mutex_lock(&h264_params_mx);
            if (!h264_params)
                h264_params = mp4mux_new(half_w, half_h);
            pthread_mutex_unlock(&h264_params_mx);
        }
    }

    if (!gpu_disabled && !gpu && (vpu || h264)) {
        if (!ipu)
            ipu = ipu_copy_open(ipu_copy_src_width(half_w), half_w,
                                half_h);
        if (!ipu) {
            gpu_disabled = 1;
            fflog(LOG_INFO, "cam: no IPU stride-fix crop, using the "
                  "NEON path");
            return;
        }
        gpu = gpu_debayer_open(eng.prof->w, eng.prof->h, hflip);
        if (!gpu) {
            gpu_disabled = 1;
            fflog(LOG_INFO, "cam: no GPU demosaic, using the NEON path");
            return;
        }
        int ok = 1;
        for (int i = 0; ok && i < eng.n_bufs; i++) {
            if (eng.bufs[i].dmabuf < 0) {
                struct v4l2_exportbuffer exp = {
                    .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                    .index = (unsigned)i,
                    .flags = O_CLOEXEC,
                };
                if (xioctl(eng.fd, VIDIOC_EXPBUF, &exp) < 0) {
                    fflog(LOG_INFO, "cam: capture dmabuf export refused: "
                          "%s", strerror(errno));
                    ok = 0;
                    break;
                }
                eng.bufs[i].dmabuf = exp.fd;
            }
            ok = gpu_debayer_attach_raw(gpu, i, eng.bufs[i].dmabuf) == 0;
        }
        for (int i = 0; ok && i < IPU_COPY_SRCS; i++) {
            int stride;
            size_t len;
            int fd = ipu_copy_src_dmabuf(ipu, i, &stride, &len);
            ok = fd >= 0 &&
                 gpu_debayer_attach_dst(gpu, i, fd, stride, len) == 0;
        }
        if (!ok) {
            gpu_debayer_close(gpu);
            gpu = NULL;
            gpu_disabled = 1;
            fflog(LOG_INFO, "cam: GPU import failed, using the NEON path");
        }
    }
}

/* One-shot GPU-versus-CPU comparison on a live frame, FORGECTRL_GPU_CHECK.
 * The GPU is not bit-identical to the NEON path (different rounding), so
 * this reports the worst per-byte difference instead of demanding zero;
 * anything beyond a couple of counts means the shader indexing is wrong
 * on this GPU. Reads the write-combine encoder buffer, so it is slow and
 * runs once. */
static void gpu_check_once(const uint8_t *raw, int raw_w, int raw_h,
                           int hflip, vpu_jpeg_t *v, int slot)
{
    static int done;
    if (done || !getenv("FORGECTRL_GPU_CHECK"))
        return;
    done = 1;

    uint8_t *gy, *gu, *gv;
    int ys, uvs;
    vpu_jpeg_planes(v, &gy, &gu, &gv, &ys, &uvs);
    const int ow = raw_w / 2, oh = raw_h / 2;
    size_t ysz = (size_t)ys * oh, usz = (size_t)uvs * (oh / 2);
    uint8_t *ry = malloc(ysz), *ru = malloc(usz), *rv = malloc(usz);
    if (ry && ru && rv) {
        debayer_bggr_half_yuv420_scalar(raw, raw_w, raw_h, hflip,
                                        ry, ys, ru, rv, uvs);
        /* Luma is held to the CPU path within rounding; chroma is
         * reported separately, because the GPU point-samples where the
         * CPU box-filters (see gpu_debayer.c), so its deltas measure
         * scene chroma detail, not correctness. */
        int dmax = 0, cmax = 0;
        long bad = 0;
        double csum = 0;
        for (int r = 0; r < oh; r++)
            for (int x = 0; x < ow; x++) {
                int d = abs((int)gy[(size_t)r * ys + x] -
                            (int)ry[(size_t)r * ys + x]);
                if (d > dmax)
                    dmax = d;
                if (d > 2)
                    bad++;
            }
        for (size_t i = 0; i < usz; i++) {
            int du = abs((int)gu[i] - (int)ru[i]);
            int dv = abs((int)gv[i] - (int)rv[i]);
            if (du > cmax)
                cmax = du;
            if (dv > cmax)
                cmax = dv;
            csum += du + dv;
        }
        fflog(LOG_INFO, "cam: GPU/CPU compare: luma max delta %d, %ld "
              "samples off by more than 2; chroma vs box filter mean "
              "%.2f max %d", dmax, bad, csum / (double)(2 * usz), cmax);

        /* Attribute any bottom-row disagreement: the same row read from
         * the IPU's source (the GPU's own output, before the crop) says
         * whether the GPU rendered it wrong or the IPU copied it wrong. */
        const uint8_t *src = ipu ? ipu_copy_src_map(ipu, slot) : NULL;
        if (src) {
            int sstride;
            size_t slen;
            ipu_copy_src_dmabuf(ipu, slot, &sstride, &slen);
            const uint8_t *pre = src + (size_t)(oh - 1) * sstride;
            const uint8_t *post = gy + (size_t)(oh - 1) * ys;
            const uint8_t *ref = ry + (size_t)(oh - 1) * ys;
            int dmax_pre = 0, dmax_post = 0;
            for (int x = 0; x < ow; x++) {
                int dp = abs((int)pre[x] - (int)ref[x]);
                int dq = abs((int)post[x] - (int)ref[x]);
                if (dp > dmax_pre)
                    dmax_pre = dp;
                if (dq > dmax_post)
                    dmax_post = dq;
            }
            fflog(LOG_INFO, "cam: bottom Y row: GPU-vs-CPU max %d, "
                  "post-IPU-vs-CPU max %d", dmax_pre, dmax_post);
        }
    }
    free(ry);
    free(ru);
    free(rv);
}

static void *worker(void *arg)
{
    (void)arg;
    /* Sized for the largest profile so a snapshot borrow of a
     * differently-modeled camera reuses them. */
    uint8_t *rgb_half = malloc(max_half_rgb_bytes());
    uint8_t *rgb_full = NULL;   /* grown on first full-res snapshot */
    size_t rgb_full_cap = 0;
    /* The 8-bit BGGR frame the demosaic paths read - the narrowing target
     * for a 10-bit sensor and the bounce buffer for an 8-bit one on
     * uncached capture buffers (see prepare_raw8). */
    uint8_t *raw_cached = malloc(max_raw8_bytes());
    double stat_dq_ms = 0, stat_copy_ms = 0, stat_conv_ms = 0,
           stat_enc_ms = 0, stat_wait_ms = 0;
    unsigned stat_n = 0;
    int dq_timeouts = 0;
    int lamp_skip = 0;      /* frames left to drain after a lamp override */
    int lamp_restore = -1;  /* engine lamp level to restore, -1 = none */
    /* FPS cap pacing (eng.fps_cap is set once at init) */
    double cap_period = eng.fps_cap > 0 ? 1.0 / eng.fps_cap : 0;
    double next_due = 0;
    struct timespec fps_t0;
    now_ts(&fps_t0);
    uint64_t fps_frames = 0;

    if (!rgb_half || !raw_cached) {
        fflog(LOG_ERR, "cam: worker OOM");
        goto out;
    }

    for (;;) {
        struct timespec now;
        now_ts(&now);
        pthread_mutex_lock(&eng.lock);
        int stop = eng.stop_flag;
        int clients = eng.clients;
        int h264c = eng.h264_clients;
        int snap = eng.snap_pending == 1 && eng.snap_cam == eng.home_cam;
        int borrow = eng.snap_pending == 1 && eng.snap_cam != eng.home_cam;
        int snap_lamp = eng.snap_lamp;
        cam_id_t borrow_cam = eng.snap_cam;
        cam_id_t orig_cam = eng.home_cam;
        int idle = clients == 0 && h264c == 0 && !snap && !borrow &&
                   ts_diff(&now, &eng.last_activity) > IDLE_STOP_S;
        pthread_mutex_unlock(&eng.lock);

        if (stop || idle)
            break;

        /* Privacy gate, re-checked every frame: a lid opened mid-capture
         * tears the pipeline down within one frame time, so streams end
         * and the sensors stop rather than filming the room. A pending
         * snapshot fails with the same refusal (see the exit path). */
        if (!machine_lid_closed()) {
            fflog(LOG_INFO, "cam: lid opened, stopping capture");
            pthread_mutex_lock(&eng.lock);
            eng.lid_stopped = 1;
            pthread_mutex_unlock(&eng.lock);
            break;
        }

        /* Same-camera snapshot with a lamp override: relight, then let
         * the in-flight frames drain before delivering; the engine level
         * is restored after delivery (or if the requester gives up). */
        if (snap && snap_lamp >= 0 && lamp_restore < 0) {
            sysfs_write_int(camdefs[orig_cam].lamp, snap_lamp);
            lamp_restore = eng.lamp_level;
            lamp_skip = LAMP_SKIP_FRAMES;
        } else if (!snap && lamp_restore >= 0) {
            sysfs_write_int(camdefs[orig_cam].lamp, lamp_restore);
            lamp_restore = -1;
        }

        /* Cross-camera snapshot: borrow the mux - pause the stream,
         * switch, grab one frame, switch back. Stream clients just see
         * the frame gap (a few seconds). */
        if (borrow) {
            char berr[128];
            release_capture();
            if (start_capture(borrow_cam, berr, sizeof(berr)) == 0) {
                int skip = 0;
                if (snap_lamp >= 0) {
                    sysfs_write_int(camdefs[borrow_cam].lamp, snap_lamp);
                    skip = LAMP_SKIP_FRAMES;
                }
                if (grab_one_snap(raw_cached, rgb_half, &rgb_full,
                                  &rgb_full_cap, skip))
                    fail_snap();
                release_capture();
            } else {
                fflog(LOG_ERR, "cam: borrow start failed: %s", berr);
                fail_snap();
            }
            if (start_capture(orig_cam, berr, sizeof(berr))) {
                fflog(LOG_ERR, "cam: restore after borrow failed: %s",
                      berr);
                break;  /* engine dies; streams end; reconnect heals */
            }
            continue;
        }

        /* Wait for a frame */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(eng.fd, &fds);
        struct timeval tv = { .tv_sec = DQ_TIMEOUT_S };
        int r = select(eng.fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1 && errno == EINTR)
            continue;
        if (r <= 0) {
            if (r == 0 && ++dq_timeouts >= MAX_DQ_TIMEOUTS) {
                fflog(LOG_WARNING, "cam: %d consecutive frame timeouts, "
                      "stopping engine", dq_timeouts);
                break;
            }
            if (r == -1) {
                fflog(LOG_ERR, "cam: select: %s", strerror(errno));
                break;
            }
            continue;
        }

        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        struct timespec c0, c1, c2;
        now_ts(&c0);    /* select() said readable: DQBUF time is not frame
                         * wait but the cache invalidate on cached buffers */
        if (xioctl(eng.fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EIO)
                continue;
            fflog(LOG_ERR, "cam: DQBUF: %s", strerror(errno));
            break;
        }
        now_ts(&c1);
        dq_timeouts = 0;

        /* An errored frame is short, torn or off-sync; demosaicing it would
         * publish a corrupt image. The ladder drops it, cycles the queue if
         * they keep coming, and gives up if cycling stops helping. */
        cam_frame_action_t act = classify(&buf);
        if (act != CAM_FRAME_USE) {
            xioctl(eng.fd, VIDIOC_QBUF, &buf);
            if (act == CAM_FRAME_WARMUP || act == CAM_FRAME_DROP)
                continue;
            if (act == CAM_FRAME_ABORT) {
                fflog(LOG_ERR, "cam: corrupt frames persist after %d stream "
                      "restarts, stopping engine", CAM_MAX_RECOVERIES);
                break;
            }
            /* A queue cycle requeues every buffer, including one held
             * for an in-flight render: settle the render first so the
             * pipeline restarts from empty. */
            if (gpu && pend_slot >= 0) {
                gpu_debayer_wait(gpu, pend_slot);
                pend_slot = -1;
            }
            if (restream()) {
                fflog(LOG_ERR, "cam: stream restart failed: %s",
                      strerror(errno));
                break;
            }
            pthread_mutex_lock(&eng.lock);
            unsigned n = eng.recoveries;
            uint64_t nbad = eng.corrupt, nall = eng.frames;
            pthread_mutex_unlock(&eng.lock);
            fflog(LOG_WARNING, "cam: corrupt frames, restarted the stream "
                  "(%u restarts, %llu of %llu frames bad)", n,
                  (unsigned long long)nbad, (unsigned long long)nall);
            continue;
        }

        /* FPS cap: a frame arriving before the next due time is requeued
         * without demosaic/encode (a pending snapshot still rides on it;
         * the sensor keeps its own pace). When the CSI hardware skip is
         * active the sensor-side rate already matches and this pacing
         * passes everything through. */
        int jpeg_want = clients > 0;
        int h264_want = h264c > 0 && !h264_disabled;
        int encode = jpeg_want || h264_want;
        if (encode && cap_period > 0) {
            double t = (double)c1.tv_sec + (double)c1.tv_nsec / 1e9;
            if (t < next_due) {
                encode = 0;
            } else {
                next_due += cap_period;
                if (next_due <= t)      /* first frame or fell behind */
                    next_due = t + cap_period;
            }
        }

        const struct sensor_profile *p = eng.prof;
        const int raw_w = p->w, raw_h = p->h;
        const int half_w = raw_w / 2, half_h = raw_h / 2;
        /* The CPU-side raw view is produced only for consumers that read
         * it (a snapshot, or a CPU demosaic); a GPU-converted stream
         * frame reads the capture buffer over its dmabuf instead. */
        const uint8_t *raw = NULL;
        if (snap)
            raw = prepare_raw8(eng.bufs[buf.index].start, raw_cached, p);
        now_ts(&c2);

        /* Snapshot request rides on the same raw frame (after any
         * lamp-change drain) */
        if (snap) {
            if (lamp_skip > 0) {
                lamp_skip--;
            } else {
                deliver_snap(raw, rgb_half, &rgb_full, &rgb_full_cap);
                if (lamp_restore >= 0) {
                    sysfs_write_int(camdefs[orig_cam].lamp, lamp_restore);
                    lamp_restore = -1;
                }
            }
        }

        /* Stream frame: demosaic + encode. On the GPU path the two are
         * pipelined: this frame's render is kicked behind a fence, and
         * the PREVIOUS frame's finished render is copied to the
         * encoders and published while the new one runs - the render
         * overlaps the copies, the encodes and the next frame wait, so
         * it alone paces the stream. The rendering frame's capture
         * buffer is held out of the queue until its fence signals. On
         * the CPU (NEON) path everything stays synchronous, and H.264
         * has no software fallback (the MJPEG stream is the fallback).
         * The pend_* state lives at file scope so teardowns reset it. */
        int hold_buf = 0;
        int want = jpeg_want || h264_want;

        if (want || pend_slot >= 0) {
            struct timespec e0, e1, e2;
            now_ts(&e0);
            if (want)
                ensure_encoders(jpeg_want, h264_want, half_w, half_h,
                                HFLIP);

            int gpu_mode = gpu && ipu;
            if (!gpu_mode)
                pend_slot = -1;     /* a teardown recycled the buffers */

            /* Collect the in-flight render: deliver it below, or drop
             * it if its viewers are gone. The held capture buffer goes
             * back to the queue as soon as the fence clears (drop) or
             * after the encoders and diagnostics are done with the
             * frame (deliver). */
            int deliver_slot = -1;
            unsigned deliver_raw = 0;
            uint64_t deliver_pts = 0;
            struct timespec g0, g1, g2;
            now_ts(&g0);
            if (gpu_mode && pend_slot >= 0) {
                int wrc = gpu_debayer_wait(gpu, pend_slot);
                if (wrc == 0 && want) {
                    deliver_slot = pend_slot;
                    deliver_raw = pend_idx;
                    deliver_pts = pend_pts;
                } else {
                    struct v4l2_buffer rb = {0};
                    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    rb.memory = V4L2_MEMORY_MMAP;
                    rb.index = pend_idx;
                    xioctl(eng.fd, VIDIOC_QBUF, &rb);
                    if (wrc != 0)
                        gpu_mode = 0;
                }
                pend_slot = -1;
            }
            now_ts(&g1);

            /* Kick this frame's render (`encode` carries the fps cap;
             * a capped frame still delivered the previous one above). */
            if (gpu_mode && encode) {
                int slot = deliver_slot == 0 ? 1 : 0;
                if (gpu_debayer_kick(gpu, (int)buf.index, slot) == 0) {
                    pend_slot = slot;
                    pend_idx = buf.index;
                    pend_pts = (uint64_t)c1.tv_sec * 90000u +
                               (uint64_t)c1.tv_nsec / 11111u;
                    hold_buf = 1;
                } else {
                    gpu_mode = 0;
                }
            }

            /* Copy the delivered render into each wanted encoder. */
            int gpu_jpeg = 0, gpu_h264 = 0;
            if (gpu_mode && deliver_slot >= 0) {
                if (jpeg_want && vpu) {
                    int stride;
                    size_t len;
                    int fd = vpu_jpeg_out_dmabuf(vpu, &stride, &len);
                    gpu_jpeg = fd >= 0 &&
                               ipu_copy_run(ipu, deliver_slot, fd,
                                            len) == 0;
                }
                if (h264_want && h264) {
                    int stride;
                    size_t len;
                    int fd = vpu_h264_out_dmabuf(h264, &stride, &len);
                    gpu_h264 = fd >= 0 &&
                               ipu_copy_run(ipu, deliver_slot, fd,
                                            len) == 0;
                }
            }
            now_ts(&g2);
            if (gpu_mode) {
                /* Fence-stall-versus-copy split: a stall near zero
                 * means the render fully overlapped the rest. */
                static double sms, cms;
                static unsigned gn;
                sms += ts_diff(&g1, &g0) * 1e3;
                cms += ts_diff(&g2, &g1) * 1e3;
                if (++gn >= 30) {
                    if (getenv("FORGECTRL_GPU_CHECK"))
                        fflog(LOG_DEBUG, "cam: gpu split: stall %.0f ms, "
                              "kick+copy %.0f ms avg", sms / gn, cms / gn);
                    sms = cms = 0;
                    gn = 0;
                }
            }
            int gpu_on = gpu_mode;
            if ((gpu || ipu) &&
                (!gpu_mode ||
                 (deliver_slot >= 0 && ((jpeg_want && vpu && !gpu_jpeg) ||
                                        (h264_want && h264 &&
                                         !gpu_h264))))) {
                gpu_debayer_close(gpu);     /* waits any pending fence */
                gpu = NULL;
                ipu_copy_close(ipu);
                ipu = NULL;
                gpu_disabled = 1;
                gpu_jpeg = gpu_h264 = 0;
                gpu_on = 0;
                if (hold_buf) {
                    struct v4l2_buffer rb = {0};
                    rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    rb.memory = V4L2_MEMORY_MMAP;
                    rb.index = pend_idx;
                    xioctl(eng.fd, VIDIOC_QBUF, &rb);
                    hold_buf = 0;
                    pend_slot = -1;
                }
                fflog(LOG_WARNING, "cam: GPU pipeline failed, "
                      "falling back to the NEON path");
            }
            /* On the GPU path with nothing delivered (the pipeline's
             * first frame), the encoders have no input this iteration. */
            int feed_encoders = !gpu_on || deliver_slot >= 0;

            /* ---- JPEG ---- */
            uint8_t *jpg = NULL;
            size_t len = 0;
            int via_vpu = 0;
            static int vpu_hard_fails;
            int vpu_rc = -1;
            if (feed_encoders && jpeg_want && vpu) {
                if (!gpu_jpeg) {
                    uint8_t *yp, *up, *vp;
                    int ys, uvs;
                    vpu_jpeg_planes(vpu, &yp, &up, &vp, &ys, &uvs);
                    if (!raw)
                        raw = prepare_raw8(eng.bufs[buf.index].start,
                                           raw_cached, p);
                    debayer_bggr_half_yuv420(raw, raw_w, raw_h, HFLIP,
                                             yp, ys, up, vp, uvs);
#ifdef __ARM_NEON
                    /* One-shot NEON-vs-scalar equivalence check on a live
                     * frame (the paths are constructed to be bit-identical;
                     * this proves it on real data). Stride == width here. */
                    static int neon_checked;
                    if (!neon_checked && getenv("FORGECTRL_NEON_CHECK")) {
                        neon_checked = 1;
                        size_t ysz = (size_t)ys * half_h;
                        size_t usz = (size_t)uvs * (half_h / 2);
                        uint8_t *ry = malloc(ysz);
                        uint8_t *ru = malloc(usz);
                        uint8_t *rv = malloc(usz);
                        if (ry && ru && rv) {
                            debayer_bggr_half_yuv420_scalar(raw, raw_w,
                                                            raw_h, HFLIP,
                                                            ry, ys,
                                                            ru, rv, uvs);
                            fflog(LOG_DEBUG, "cam: NEON/scalar compare: %s",
                                  (!memcmp(ry, yp, ysz) &&
                                     !memcmp(ru, up, usz) &&
                                     !memcmp(rv, vp, usz))
                                  ? "IDENTICAL" : "MISMATCH");
                        }
                        free(ry);
                        free(ru);
                        free(rv);
                    }
#endif
                } else if (getenv("FORGECTRL_GPU_CHECK")) {
                    /* The encoder holds the DELIVERED frame; compare it
                     * against that frame's raw (still held). */
                    gpu_check_once(prepare_raw8(eng.bufs[deliver_raw].start,
                                                raw_cached, p),
                                   raw_w, raw_h, HFLIP, vpu, deliver_slot);
                }
                now_ts(&e1);
                vpu_rc = vpu_jpeg_encode(vpu, &jpg, &len);
                if (vpu_rc == 0) {
                    via_vpu = 1;
                    vpu_hard_fails = 0;
                } else if (vpu_rc > 0) {
                    /* transient errored frame (e.g. bitstream overflow
                     * on a noise frame): drop it, keep the VPU */
                    vpu_hard_fails = 0;
                } else if (++vpu_hard_fails >= 3) {
                    fflog(LOG_WARNING, "cam: repeated VPU encode failures, "
                          "falling back to software");
                    vpu_jpeg_close(vpu);
                    vpu = NULL;
                    vpu_disabled = 1;
                }
            }
            if (feed_encoders && jpeg_want && !via_vpu && vpu_rc < 0) {
                if (!raw)
                    raw = prepare_raw8(eng.bufs[buf.index].start,
                                       raw_cached, p);
                debayer_bggr_half(raw, rgb_half, raw_w, raw_h, HFLIP);
                now_ts(&e1);
                if (jpeg_encode_rgb(rgb_half, half_w, half_h,
                                    eng.stream_quality, 1, &jpg, &len))
                    jpg = NULL;
            }
            now_ts(&e2);

            /* ---- H.264 ---- */
            if (feed_encoders && h264_want && h264) {
                if (!gpu_h264) {
                    uint8_t *yp, *up, *vp;
                    int ys, uvs;
                    vpu_h264_planes(h264, &yp, &up, &vp, &ys, &uvs);
                    if (!raw)
                        raw = prepare_raw8(eng.bufs[buf.index].start,
                                           raw_cached, p);
                    /* With both stream types on the CPU path this is a
                     * second demosaic of the same frame; the GPU path is
                     * how that cost is meant to be paid. */
                    debayer_bggr_half_yuv420(raw, raw_w, raw_h, HFLIP,
                                             yp, ys, up, vp, uvs);
                }
                pthread_mutex_lock(&eng.lock);
                int want_key = eng.h264_key_req;
                eng.h264_key_req = 0;
                pthread_mutex_unlock(&eng.lock);
                if (want_key)
                    vpu_h264_force_key(h264);

                uint8_t *au = NULL;
                size_t aulen = 0;
                int key = 0;
                static int h264_hard_fails;
                int rc = vpu_h264_encode(h264, &au, &aulen, &key);
                if (rc == 0) {
                    h264_hard_fails = 0;
                    pthread_mutex_lock(&h264_params_mx);
                    if (h264_params)
                        mp4mux_feed_params(h264_params, au, aulen);
                    pthread_mutex_unlock(&h264_params_mx);
                    pthread_mutex_lock(&eng.lock);
                    free(eng.h264_au);
                    eng.h264_au = au;
                    eng.h264_len = aulen;
                    eng.h264_key = key;
                    eng.h264_pts = gpu_h264 ? deliver_pts
                                            : (uint64_t)c1.tv_sec * 90000u +
                                              (uint64_t)c1.tv_nsec / 11111u;
                    eng.h264_seq++;
                    eng.h264_up = 1;
                    eng.gpu_active = gpu_jpeg || gpu_h264;
                    pthread_cond_broadcast(&eng.h264_cv);
                    pthread_mutex_unlock(&eng.lock);
                } else if (rc < 0) {
                    if (want_key) {
                        pthread_mutex_lock(&eng.lock);
                        eng.h264_key_req = 1;   /* not consumed */
                        pthread_mutex_unlock(&eng.lock);
                    }
                    if (++h264_hard_fails >= 3) {
                        fflog(LOG_WARNING, "cam: repeated H.264 encode "
                              "failures, dropping the H.264 stream");
                        vpu_h264_close(h264);
                        h264 = NULL;
                        h264_disabled = 1;
                        pthread_mutex_lock(&eng.lock);
                        eng.h264_up = 0;
                        pthread_cond_broadcast(&eng.h264_cv);
                        pthread_mutex_unlock(&eng.lock);
                    }
                }
            }

            /* The delivered frame's capture buffer goes back now: the
             * encoders and the diagnostics are done reading it. */
            if (deliver_slot >= 0) {
                struct v4l2_buffer rb = {0};
                rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                rb.memory = V4L2_MEMORY_MMAP;
                rb.index = deliver_raw;
                if (xioctl(eng.fd, VIDIOC_QBUF, &rb) < 0)
                    fflog(LOG_ERR, "cam: delivered-frame QBUF: %s",
                          strerror(errno));
            }

            if (jpg) {
                stat_wait_ms += ts_diff(&c0, &now) * 1e3;
                stat_dq_ms += ts_diff(&c1, &c0) * 1e3;
                stat_copy_ms += ts_diff(&c2, &c1) * 1e3;
                stat_conv_ms += ts_diff(&e1, &e0) * 1e3;
                stat_enc_ms += ts_diff(&e2, &e1) * 1e3;
                /* ~every 11 min of streaming, not every ~7 s: LightBurn
                 * keeps a stream open for whole sessions, and /data is
                 * the persistent partition settings and updates live on.
                 * Under the GPU diagnostic env the cadence tightens so a
                 * bench drill sees the split without waiting. */
                if (++stat_n >= (getenv("FORGECTRL_GPU_CHECK") ? 30u
                                                               : 10000u)) {
                    fflog(LOG_DEBUG, "cam: stream stats: wait %.0f ms, "
                          "dqbuf %.0f ms, copy %.0f ms, convert %.0f ms, "
                          "encode %.0f ms avg (%s, %s)",
                          stat_wait_ms / stat_n,
                          stat_dq_ms / stat_n, stat_copy_ms / stat_n,
                          stat_conv_ms / stat_n, stat_enc_ms / stat_n,
                          via_vpu ? "vpu" : "software",
                          eng.cached_bufs ? "cached" : "uncached");
                    stat_dq_ms = stat_copy_ms = stat_conv_ms = 0;
                    stat_enc_ms = stat_wait_ms = 0;
                    stat_n = 0;
                }
                pthread_mutex_lock(&eng.lock);
                free(eng.stream_jpg);
                eng.stream_jpg = jpg;
                eng.stream_len = len;
                eng.vpu_active = via_vpu;
                eng.gpu_active = gpu_jpeg || gpu_h264;
                eng.seq++;
                fps_frames++;
                struct timespec t;
                now_ts(&t);
                double dt = ts_diff(&t, &fps_t0);
                if (dt >= 2.0) {
                    eng.fps = (double)fps_frames / dt;
                    fps_frames = 0;
                    fps_t0 = t;
                }
                pthread_cond_broadcast(&eng.frame_cv);
                pthread_mutex_unlock(&eng.lock);
            }
        }

        /* A frame whose render is in flight stays out of the queue; it
         * goes back when its fence clears on a later iteration. */
        if (!hold_buf && xioctl(eng.fd, VIDIOC_QBUF, &buf) < 0) {
            fflog(LOG_ERR, "cam: QBUF: %s", strerror(errno));
            break;
        }
    }

out:
    release_capture();
    free(rgb_half);
    free(rgb_full);
    free(raw_cached);
    pthread_mutex_lock(&eng.lock);
    eng.running = 0;
    eng.h264_up = 0;
    /* fail any waiter: stream clients see running==0, a pending snapshot
     * is marked failed */
    if (eng.snap_pending == 1)
        eng.snap_pending = 3;
    pthread_cond_broadcast(&eng.frame_cv);
    pthread_cond_broadcast(&eng.snap_cv);
    pthread_cond_broadcast(&eng.h264_cv);
    pthread_mutex_unlock(&eng.lock);
    return NULL;
}

/* ------------------------------------------------------- engine control */

/* With ctl held: make the engine run on `cam`. Fails if clients hold the
 * other camera. */
static int ensure_engine(cam_id_t cam, char *err, size_t errlen)
{
    for (;;) {
        pthread_mutex_lock(&eng.lock);
        int running = eng.running;
        /* Compare against the HOME camera: during a snapshot borrow the
         * pipeline (eng.cam) is briefly on the other sensor, and a stream
         * request racing that window must not attach to it. */
        cam_id_t cur = eng.home_cam;
        int clients = eng.clients + eng.h264_clients;
        int tid_valid = eng.tid_valid;
        pthread_mutex_unlock(&eng.lock);

        if (running && cur == cam)
            return 0;

        if (running && cur != cam) {
            if (clients > 0) {
                /* Last request wins: preempt the current stream clients
                 * (single-operator machine - the newest ask is the
                 * operator). Kicked clients wake, end their streams
                 * cleanly (viewers freeze on their last frame), and
                 * release their pins; wait for that to drain. */
                pthread_mutex_lock(&eng.lock);
                eng.kick_gen++;
                pthread_cond_broadcast(&eng.frame_cv);
                pthread_cond_broadcast(&eng.h264_cv);
                pthread_mutex_unlock(&eng.lock);
                struct timespec t0, t;
                now_ts(&t0);
                do {
                    usleep(100 * 1000);
                    pthread_mutex_lock(&eng.lock);
                    clients = eng.clients + eng.h264_clients;
                    pthread_mutex_unlock(&eng.lock);
                    now_ts(&t);
                } while (clients > 0 && ts_diff(&t, &t0) < SWITCH_GRACE_S);
                if (clients > 0) {
                    snprintf(err, errlen,
                             "camera switch timed out: %d client(s) still "
                             "attached to %s", clients, camdefs[cur].name);
                    return -1;
                }
            }
            pthread_mutex_lock(&eng.lock);
            eng.stop_flag = 1;
            pthread_mutex_unlock(&eng.lock);
            /* worker notices at the next tick (<= DQ_TIMEOUT_S) */
        }

        if (tid_valid) {
            pthread_join(eng.tid, NULL);
            pthread_mutex_lock(&eng.lock);
            eng.tid_valid = 0;
            eng.stop_flag = 0;
            pthread_mutex_unlock(&eng.lock);
            continue;   /* re-evaluate from a clean state */
        }

        /* cold start */
        pthread_mutex_lock(&eng.lock);
        eng.home_cam = cam;
        pthread_mutex_unlock(&eng.lock);
        if (start_capture(cam, err, errlen))
            return -1;
        pthread_mutex_lock(&eng.lock);
        eng.running = 1;
        eng.stop_flag = 0;
        eng.seq = 0;
        eng.fps = 0;
        now_ts(&eng.last_activity);
        if (pthread_create(&eng.tid, NULL, worker, NULL)) {
            eng.running = 0;
            pthread_mutex_unlock(&eng.lock);
            release_capture();
            snprintf(err, errlen, "worker thread creation failed");
            return -1;
        }
        eng.tid_valid = 1;
        pthread_mutex_unlock(&eng.lock);
        return 0;
    }
}

void cam_engine_init(void)
{
    const char *v;
    if ((v = getenv("FORGECTRL_STREAM_Q")) != NULL) {
        int q = atoi(v);
        if (q >= 1 && q <= 100)
            eng.stream_quality = q;
    }
    if ((v = getenv("FORGECTRL_LAMP")) != NULL) {
        int l = atoi(v);
        if (l >= 0 && l <= 1023)
            eng.lamp_level = l;
    }
    if ((v = getenv("FORGECTRL_STREAM_FPS")) != NULL) {
        double f = atof(v);
        if (f > 0 && f <= 60)
            eng.fps_cap = f;
    }
    if ((v = getenv("FORGECTRL_H264_KBPS")) != NULL) {
        int k = atoi(v);
        if (k >= 100 && k <= 20000)
            eng.h264_kbps = k;
    }
    if ((v = getenv("FORGECTRL_H264_GOP")) != NULL) {
        int gp = atoi(v);
        if (gp >= 1 && gp <= 300)
            eng.h264_gop = gp;
    }
    if (getenv("FORGECTRL_NO_VPU"))
        vpu_disabled = 1;
    if (getenv("FORGECTRL_NO_H264"))
        h264_disabled = 1;
    if (getenv("FORGECTRL_NO_GPU"))
        gpu_disabled = 1;
    if (getenv("FORGECTRL_NO_HW_SKIP"))
        hw_skip_disabled = 1;
    if (getenv("FORGECTRL_NO_CACHED_BUFS"))
        cached_disabled = 1;

    /* Resolve which sensor bound on each bus now, so /cam/status can name
     * the model and its geometry before the engine has ever run. Absent or
     * unrecognized is not an error here - start_capture is where that
     * matters. Refreshed on every engine start. */
    for (int i = 0; i < 2; i++) {
        char entity[64];
        if (sensor_entity(camdefs[i].bus, entity, sizeof(entity)))
            continue;
        eng.seen[i] = profile_for(entity);
        fflog(LOG_INFO, "cam: %s camera is %s", camdefs[i].name,
              eng.seen[i] ? eng.seen[i]->model : entity);
    }
}

void cam_engine_shutdown(void)
{
    pthread_mutex_lock(&eng.ctl);
    pthread_mutex_lock(&eng.lock);
    int tid_valid = eng.tid_valid;
    eng.stop_flag = 1;
    pthread_mutex_unlock(&eng.lock);
    if (tid_valid) {
        pthread_join(eng.tid, NULL);
        pthread_mutex_lock(&eng.lock);
        eng.tid_valid = 0;
        pthread_mutex_unlock(&eng.lock);
    }
    if (vpu) {
        vpu_jpeg_close(vpu);
        vpu = NULL;
    }
    if (h264) {
        vpu_h264_close(h264);
        h264 = NULL;
    }
    if (ipu) {
        ipu_copy_close(ipu);
        ipu = NULL;
    }
    pthread_mutex_lock(&h264_params_mx);
    mp4mux_free(h264_params);
    h264_params = NULL;
    pthread_mutex_unlock(&h264_params_mx);
    pthread_mutex_unlock(&eng.ctl);
}

/* ------------------------------------------------------------ snapshots */

int cam_snapshot(cam_id_t cam, int full, int quality, int lamp,
                 uint8_t **jpeg, size_t *len, char *err, size_t errlen)
{
    /* Refuse before taking the control mutex: an open lid is answered
     * immediately, not after the snapshot timeout. start_capture() and
     * the worker enforce the same rule, so a lid that opens during the
     * wait still ends the request. */
    if (!machine_lid_closed()) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return -1;
    }

    pthread_mutex_lock(&eng.ctl);

    /* If the engine is streaming the OTHER camera for active clients,
     * don't switch it - post the request and let the worker borrow the
     * mux for one frame. Otherwise make the engine run on `cam`. */
    pthread_mutex_lock(&eng.lock);
    int streaming_other = eng.running && eng.home_cam != cam &&
                          eng.clients > 0;
    pthread_mutex_unlock(&eng.lock);

    if (!streaming_other && ensure_engine(cam, err, errlen)) {
        pthread_mutex_unlock(&eng.ctl);
        return -1;
    }

    pthread_mutex_lock(&eng.lock);
    eng.snap_pending = 1;
    eng.snap_cam = cam;
    eng.snap_full = full;
    eng.snap_quality = quality;
    eng.snap_lamp = lamp;
    now_ts(&eng.last_activity);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SNAP_TIMEOUT_S;
    int rc = 0;
    while (eng.snap_pending == 1) {
        if (pthread_cond_timedwait(&eng.snap_cv, &eng.lock, &deadline)
            == ETIMEDOUT) {
            rc = ETIMEDOUT;
            break;
        }
    }
    if (rc == 0 && eng.snap_pending == 2) {
        *jpeg = eng.snap_jpg;
        *len = eng.snap_len;
        eng.snap_jpg = NULL;
        eng.snap_len = 0;
        eng.snap_pending = 0;
    } else if (eng.lid_stopped) {
        /* The lid opened while this snapshot was in flight: report the
         * refusal rather than a generic failure, so the caller sees the
         * same answer it would have got a moment earlier. */
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        eng.snap_pending = 0;
        rc = -1;
    } else {
        if (eng.snap_pending == 1 || eng.snap_pending == 3)
            snprintf(err, errlen, rc == ETIMEDOUT ?
                     "snapshot timed out" : "snapshot capture failed");
        eng.snap_pending = 0;
        rc = -1;
    }
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    pthread_mutex_unlock(&eng.ctl);
    return rc == 0 ? 0 : -1;
}

/* --------------------------------------------------------- stream client */

struct cam_client {
    uint64_t last_seq;
    uint64_t gen;       /* kick generation at open; a bump ends the stream */
    uint8_t *buf;
    size_t   cap;
};

cam_client_t *cam_client_open(cam_id_t cam, char *err, size_t errlen)
{
    if (!machine_lid_closed()) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return NULL;
    }
    pthread_mutex_lock(&eng.ctl);
    if (ensure_engine(cam, err, errlen)) {
        pthread_mutex_unlock(&eng.ctl);
        return NULL;
    }
    cam_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        pthread_mutex_unlock(&eng.ctl);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    pthread_mutex_lock(&eng.lock);
    eng.clients++;
    c->gen = eng.kick_gen;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    pthread_mutex_unlock(&eng.ctl);
    return c;
}

long cam_client_next(cam_client_t *c, const uint8_t **jpeg)
{
    pthread_mutex_lock(&eng.lock);
    int timeouts = 0;
    while (eng.running && c->gen == eng.kick_gen && eng.seq <= c->last_seq) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += CLIENT_WAIT_S;
        if (pthread_cond_timedwait(&eng.frame_cv, &eng.lock, &deadline)
            == ETIMEDOUT && ++timeouts >= 2)
            break;
    }
    if (!eng.running || c->gen != eng.kick_gen || eng.seq <= c->last_seq) {
        pthread_mutex_unlock(&eng.lock);
        return -1;
    }
    if (c->cap < eng.stream_len) {
        uint8_t *nb = realloc(c->buf, eng.stream_len);
        if (!nb) {
            pthread_mutex_unlock(&eng.lock);
            return -1;
        }
        c->buf = nb;
        c->cap = eng.stream_len;
    }
    memcpy(c->buf, eng.stream_jpg, eng.stream_len);
    long len = (long)eng.stream_len;
    c->last_seq = eng.seq;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    *jpeg = c->buf;
    return len;
}

void cam_client_close(cam_client_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&eng.lock);
    if (eng.clients > 0)
        eng.clients--;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    free(c->buf);
    free(c);
}

/* ---------------------------------------------------- H.264 stream client */

struct cam_h264_client {
    uint64_t last_seq;
    uint64_t gen;
    int      started;   /* first delivered unit must be an IDR */
    uint8_t *buf;
    size_t   cap;
};

cam_h264_client_t *cam_h264_client_open(cam_id_t cam, char *err,
                                        size_t errlen)
{
    if (!machine_lid_closed()) {
        snprintf(err, errlen, "%s", CAM_ERR_LID);
        return NULL;
    }
    pthread_mutex_lock(&eng.ctl);
    if (ensure_engine(cam, err, errlen)) {
        pthread_mutex_unlock(&eng.ctl);
        return NULL;
    }
    cam_h264_client_t *c = calloc(1, sizeof(*c));
    if (!c) {
        pthread_mutex_unlock(&eng.ctl);
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    pthread_mutex_lock(&eng.lock);
    eng.h264_clients++;
    eng.h264_key_req = 1;   /* this viewer needs an IDR to start on */
    c->gen = eng.kick_gen;
    c->last_seq = eng.h264_seq;     /* only frames from now on */
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    pthread_mutex_unlock(&eng.ctl);
    return c;
}

long cam_h264_next(cam_h264_client_t *c, const uint8_t **au,
                   uint64_t *pts90k, int *key)
{
    pthread_mutex_lock(&eng.lock);
    for (;;) {
        int timeouts = 0;
        while (eng.running && !h264_disabled && c->gen == eng.kick_gen &&
               eng.h264_seq <= c->last_seq) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += CLIENT_WAIT_S;
            if (pthread_cond_timedwait(&eng.h264_cv, &eng.lock, &deadline)
                == ETIMEDOUT && ++timeouts >= 2)
                break;
        }
        if (!eng.running || h264_disabled || c->gen != eng.kick_gen ||
            eng.h264_seq <= c->last_seq) {
            pthread_mutex_unlock(&eng.lock);
            return -1;
        }
        c->last_seq = eng.h264_seq;
        if (!c->started && !eng.h264_key)
            continue;       /* wait for this viewer's IDR */
        break;
    }
    c->started = 1;
    if (c->cap < eng.h264_len) {
        uint8_t *nb = realloc(c->buf, eng.h264_len);
        if (!nb) {
            pthread_mutex_unlock(&eng.lock);
            return -1;
        }
        c->buf = nb;
        c->cap = eng.h264_len;
    }
    memcpy(c->buf, eng.h264_au, eng.h264_len);
    long len = (long)eng.h264_len;
    *pts90k = eng.h264_pts;
    *key = eng.h264_key;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    *au = c->buf;
    return len;
}

void cam_h264_client_close(cam_h264_client_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&eng.lock);
    if (eng.h264_clients > 0)
        eng.h264_clients--;
    now_ts(&eng.last_activity);
    pthread_mutex_unlock(&eng.lock);
    free(c->buf);
    free(c);
}

size_t cam_h264_params(uint8_t *buf, size_t cap)
{
    size_t n = 0;
    pthread_mutex_lock(&h264_params_mx);
    if (h264_params)
        n = mp4mux_params_annexb(h264_params, buf, cap);
    pthread_mutex_unlock(&h264_params_mx);
    return n;
}

/* --------------------------------------------------------------- status */

void cam_get_status(struct cam_status *st)
{
    pthread_mutex_lock(&eng.lock);
    st->running = eng.running;
    st->cam = eng.home_cam;
    st->clients = eng.clients;
    st->seq = eng.seq;
    st->fps = eng.fps;
    st->fps_cap = eng.fps_cap;
    st->vpu = eng.vpu_active;
    st->gpu = eng.gpu_active;
    st->hw_skip = eng.hw_skip;
    st->cached = eng.cached_bufs;
    st->h264_up = eng.h264_up;
    st->h264_clients = eng.h264_clients;
    /* Geometry follows the sensor the served camera actually carries. */
    const struct sensor_profile *p = eng.seen[eng.home_cam];
    st->sensor = p ? p->model : "unknown";
    st->snap_w = p ? p->w : 0;
    st->snap_h = p ? p->h : 0;
    st->stream_w = p ? p->w / 2 : 0;
    st->stream_h = p ? p->h / 2 : 0;
    st->lid_stopped = eng.lid_stopped;
    st->frames = eng.frames;
    st->corrupt = eng.corrupt;
    st->recoveries = eng.recoveries;
    pthread_mutex_unlock(&eng.lock);
    /* Read outside the lock: it opens a device, and nothing else here
     * depends on it being sampled at the same instant as the counters. */
    st->lid_closed = machine_lid_closed();
}

/*
 * curverec.c - the owner-run dose-curve recorder
 *
 * The tube's light output is convex in pulse density, so the controller
 * maps a commanded power through a measured curve (laser_dose_curve).
 * That curve is per tube and supply and drifts with tube age; this
 * module lets an owner measure their own, with no new emission path:
 * the panel hands them a ladder G-code file, they run it from their own
 * sender and press the button as for any job - every arm gate and
 * interlock stands - while forgectrl passively samples the tube current
 * (pic/hv_current) and the head thermopile (head/beam_detect_analog,
 * a scatter detector in the beam path, so it reads the beam and not the
 * material) at 25 Hz. When the ladder has played, the fitter segments
 * the trace on the dark gaps, reads each rung's thermopile delta over
 * its local baseline, normalizes to the full-power rung, and offers the
 * result as a ready laser_dose_curve value the panel can apply.
 *
 * For the measurement to be the raw dose response, the floor and the
 * curve in force must not bend the ladder: start saves
 * laser_floor_density and laser_dose_curve, writes 0 and "off", and
 * every end path (fit, stop, failure) restores them. The controller
 * re-reads both at each arm, so the override applies to the ladder job
 * and to nothing after it. Re-running the ladder over time gives the
 * tube's aging as a trend.
 *
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include "curverec.h"
#include "fflog.h"
#include "settings.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_HZ      25.0
#define MAX_SAMPLES    30000            /* 20 minutes at 25 Hz */
#define HV_ON          30               /* discharge present above this */
#define GAP_MERGE_S    1.0             /* dark shorter than this stays in a rung */
#define MIN_SEG_S      2.0             /* a rung is at least this long lit */
#define DARK_END_S     20.0            /* recording ends after this much dark */
#define WAIT_TIMEOUT_S 600.0           /* budget for the arm + the press */
#define RUN_TIMEOUT_S  900.0

/* The ladder's rungs: the S each maps to with the floor at 0 and the
 * curve off is the density itself, so the fitter knows each segment's
 * density by position. Matched to curverec_ladder_gcode(). */
static const double LADDER_D[] = { 10, 20, 30, 45, 60, 80, 100 };
#define LADDER_N ((int)(sizeof(LADDER_D) / sizeof(LADDER_D[0])))

enum { CR_IDLE, CR_WAITING, CR_RECORDING, CR_DONE, CR_FAILED };

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int cr_state = CR_IDLE;
static char cr_reason[128];
static time_t cr_started;
static int cr_samples;
static long *buf_hv, *buf_tp;
static pthread_t thread;
static int thread_live;
static volatile int stop_requested;
static char saved_floor[32], saved_curve[192];
static char result_curve[256];
static curverec_pt result_pts[LADDER_N];
static int result_n;

static const char *sysfs_root(void)
{
    const char *r = getenv("GF_SYSFS_ROOT");
    return r && *r ? r : "/sys/glowforge";
}

static long rd_long(const char *attr, long fallback)
{
    char path[192], text[32];
    snprintf(path, sizeof(path), "%s/%s", sysfs_root(), attr);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fallback;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return fallback;
    text[n] = '\0';
    return atol(text);
}

/* -------------------------------------------------- the pure fitter */

static double win_mean(const long *v, int a, int b)
{
    if (b <= a)
        return NAN;
    double s = 0;
    for (int i = a; i < b; i++)
        s += (double)v[i];
    return s / (b - a);
}

int curverec_fit(const long *hv, const long *tp, int n, double hz,
                 curverec_pt *pts, int max_pts, char *err, size_t elen)
{
    int gap_merge = (int)(GAP_MERGE_S * hz);
    int min_seg = (int)(MIN_SEG_S * hz);
    int seg_a[LADDER_N + 4], seg_b[LADDER_N + 4];
    int nseg = 0, start = -1, last_on = -1;

    for (int i = 0; i <= n; i++) {
        int on = i < n && hv[i] > HV_ON;
        if (on) {
            if (start < 0)
                start = i;
            last_on = i;
        } else if (start >= 0 && (i == n || i - last_on > gap_merge)) {
            if (last_on - start >= min_seg) {
                if (nseg < LADDER_N + 4) {
                    seg_a[nseg] = start;
                    seg_b[nseg] = last_on + 1;
                }
                nseg++;
            }
            start = -1;
        }
    }
    if (nseg != LADDER_N) {
        snprintf(err, elen, "%d discharge segments, the ladder has %d rungs",
                 nseg, LADDER_N);
        return -1;
    }

    double delta[LADDER_N];
    for (int s = 0; s < LADDER_N; s++) {
        int b0 = seg_a[s] - (int)(1.5 * hz);
        int b1 = seg_a[s] - (int)(0.5 * hz);
        if (b0 < 0)
            b0 = 0;
        double base = win_mean(tp, b0, b1);
        double lit = win_mean(tp, seg_a[s] + (int)(0.5 * hz),
                              seg_b[s] - (int)(0.3 * hz));
        if (isnan(base) || isnan(lit)) {
            snprintf(err, elen, "rung %d too short to read", s + 1);
            return -1;
        }
        delta[s] = lit - base;
    }
    if (delta[LADDER_N - 1] <= 0) {
        snprintf(err, elen, "the full-power rung shows no thermopile rise");
        return -1;
    }
    for (int s = 1; s < LADDER_N; s++) {
        if (delta[s] <= delta[s - 1]) {
            snprintf(err, elen, "rung %d does not rise over rung %d: not a "
                     "clean ladder", s + 1, s);
            return -1;
        }
    }
    int out = LADDER_N < max_pts ? LADDER_N : max_pts;
    for (int s = 0; s < out; s++) {
        pts[s].density = LADDER_D[s];
        pts[s].light = delta[s] / delta[LADDER_N - 1] * 100.0;
        if (pts[s].light < 0.01)
            pts[s].light = 0.01;        /* keep the pair strictly increasing */
    }
    return out;
}

/* -------------------------------------------------- lifecycle */

static void restore_keys_locked(void)
{
    settings_set("laser_floor_density", saved_floor);
    settings_set("laser_dose_curve", saved_curve);
}

static void finish_locked(int state, const char *reason)
{
    cr_state = state;
    snprintf(cr_reason, sizeof(cr_reason), "%s", reason ? reason : "");
    restore_keys_locked();
    fflog(LOG_INFO, "curverec: %s%s%s",
          state == CR_DONE ? "done" : "failed",
          reason && *reason ? " - " : "", reason ? reason : "");
}

static void fit_locked(void)
{
    char err[96] = "";
    int n = curverec_fit(buf_hv, buf_tp, cr_samples, SAMPLE_HZ,
                         result_pts, LADDER_N, err, sizeof(err));
    if (n < 0) {
        finish_locked(CR_FAILED, err);
        return;
    }
    result_n = n;
    size_t off = 0;
    result_curve[0] = '\0';
    for (int i = 0; i < n; i++)
        off += (size_t)snprintf(result_curve + off, sizeof(result_curve) - off,
                                "%s%g:%.2f", i ? "," : "",
                                result_pts[i].density, result_pts[i].light);
    finish_locked(CR_DONE, "");
}

static void *record_thread(void *arg)
{
    (void)arg;
    struct timespec tick = { 0, (long)(1e9 / SAMPLE_HZ) };
    int lit_seen = 0, dark_run = 0;
    time_t t0 = time(NULL);

    for (;;) {
        long hv = rd_long("pic/hv_current", 0);
        long tp = rd_long("head/beam_detect_analog", 0);

        pthread_mutex_lock(&mu);
        if (stop_requested) {
            if (lit_seen)
                fit_locked();
            else
                finish_locked(CR_FAILED, "stopped before the ladder fired");
            pthread_mutex_unlock(&mu);
            break;
        }
        if (cr_samples < MAX_SAMPLES) {
            buf_hv[cr_samples] = hv;
            buf_tp[cr_samples] = tp;
            cr_samples++;
        }
        if (hv > HV_ON) {
            if (!lit_seen) {
                lit_seen = 1;
                cr_state = CR_RECORDING;
                fflog(LOG_INFO, "curverec: the ladder is firing");
            }
            dark_run = 0;
        } else if (lit_seen) {
            dark_run++;
        }
        double elapsed = difftime(time(NULL), t0);
        if (lit_seen && dark_run > (int)(DARK_END_S * SAMPLE_HZ)) {
            fit_locked();
            pthread_mutex_unlock(&mu);
            break;
        }
        if ((!lit_seen && elapsed > WAIT_TIMEOUT_S) ||
            elapsed > RUN_TIMEOUT_S || cr_samples >= MAX_SAMPLES) {
            finish_locked(CR_FAILED, lit_seen ? "the record ran out of room"
                                              : "no discharge seen: the ladder never ran");
            pthread_mutex_unlock(&mu);
            break;
        }
        pthread_mutex_unlock(&mu);
        nanosleep(&tick, NULL);
    }
    return NULL;
}

int curverec_start(char *err, size_t elen)
{
    pthread_mutex_lock(&mu);
    if (cr_state == CR_WAITING || cr_state == CR_RECORDING) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "a recording is already running");
        return -1;
    }
    if (thread_live) {
        pthread_join(thread, NULL);
        thread_live = 0;
    }
    if (!buf_hv)
        buf_hv = calloc(MAX_SAMPLES, sizeof(long));
    if (!buf_tp)
        buf_tp = calloc(MAX_SAMPLES, sizeof(long));
    if (!buf_hv || !buf_tp) {
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "out of memory");
        return -1;
    }
    if (settings_get("laser_floor_density", saved_floor, sizeof(saved_floor)) != 0)
        saved_floor[0] = '\0';
    if (settings_get("laser_dose_curve", saved_curve, sizeof(saved_curve)) != 0)
        saved_curve[0] = '\0';
    settings_set("laser_floor_density", "0");
    settings_set("laser_dose_curve", "off");
    cr_state = CR_WAITING;
    cr_reason[0] = '\0';
    cr_samples = 0;
    result_n = 0;
    result_curve[0] = '\0';
    stop_requested = 0;
    cr_started = time(NULL);
    if (pthread_create(&thread, NULL, record_thread, NULL) != 0) {
        finish_locked(CR_FAILED, "cannot start the sampler");
        pthread_mutex_unlock(&mu);
        snprintf(err, elen, "cannot start the sampler");
        return -1;
    }
    thread_live = 1;
    pthread_mutex_unlock(&mu);
    fflog(LOG_INFO, "curverec: recording armed - run the ladder from the sender "
          "(floor and curve overridden for the run)");
    return 0;
}

void curverec_stop(void)
{
    pthread_mutex_lock(&mu);
    int live = cr_state == CR_WAITING || cr_state == CR_RECORDING;
    if (live)
        stop_requested = 1;
    pthread_mutex_unlock(&mu);
    if (live && thread_live) {
        pthread_join(thread, NULL);
        thread_live = 0;
    }
}

int curverec_status_json(char *buf, size_t len)
{
    static const char *names[] = { "idle", "waiting", "recording", "done", "failed" };
    pthread_mutex_lock(&mu);
    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off,
        "{\"state\":\"%s\",\"reason\":\"%s\",\"elapsed_s\":%ld,"
        "\"samples\":%d,\"curve\":\"%s\",\"points\":[",
        names[cr_state], cr_reason,
        cr_state == CR_WAITING || cr_state == CR_RECORDING
            ? (long)difftime(time(NULL), cr_started) : 0,
        cr_samples, result_curve);
    for (int i = 0; i < result_n && off < len - 48; i++)
        off += (size_t)snprintf(buf + off, len - off,
            "%s{\"density\":%g,\"light\":%.2f}", i ? "," : "",
            result_pts[i].density, result_pts[i].light);
    snprintf(buf + off, len - off, "]}");
    pthread_mutex_unlock(&mu);
    return 0;
}

int curverec_ladder_gcode(char *buf, size_t len)
{
    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off,
        "; ForgeFIRM dose-curve ladder\n"
        "; Run this file from your sender with scrap under the head:\n"
        "; 100 mm of free +X travel and %d mm of +Y. Start a recording on\n"
        "; the control panel first, then run this and press the button.\n"
        "; One line per rung, low to full power, laser off between rungs.\n"
        "G21\nG91\nM3\n", LADDER_N);
    for (int i = 0; i < LADDER_N; i++)
        off += (size_t)snprintf(buf + off, len - off,
            "S%d\nG1 X%d F600\nG0 Y1\n",
            (int)(LADDER_D[i] * 10.0), i % 2 == 0 ? 100 : -100);
    off += (size_t)snprintf(buf + off, len - off, "M5\nG90\nM2\n");
    return off < len ? 0 : -1;
}

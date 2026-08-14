/*
 * liveness.c - forgectrl: motion-liveness probe
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The DRV8825 stepper drivers on the factory board do not tolerate
 * glitches on the 40 V motor rail: a glitch can leave them in an
 * unserviceable state in which SDMA playback and the position counters
 * run normally while the motors produce nothing. The drivers' reset
 * lines are strapped (no kernel pin), their fault lines do not flag
 * the state, and no counter or register distinguishes it - the ONLY
 * software-visible truth is the head accelerometer, which physically
 * rides the gantry.
 *
 * The probe: write a small X move to the pulse ring through the
 * broker's fd - RIGHT (+X) first, then back, always: a cable lives at
 * the end of LEFT travel and must never be crushed - run it, and
 * sample the head accelerometer through the window. Real motion shows
 * start/stop transients and travel vibration (bench-characterized:
 * peak-to-peak >= ~1000 counts on X during an identical commanded
 * move); wedged drivers show the resting noise floor (p2p <= ~210).
 * The thresholds sit >= 2x from both measured sides.
 *
 * The laser latch stays locked (the probe never touches it), the move
 * is +/-15 mm relative at ~10 mm/s, and the position counters return
 * to their starting values (equal steps out and back). The kernel is
 * left with the program cleared; the controller that starts afterwards
 * reconfigures everything at its own init as usual.
 */
#define _GNU_SOURCE
#include "liveness.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* EV_SW bits (device tree): 3 = doors (combined), 5 = remote interlock
 * (active-when-open). The probe moves the gantry, so it must never run
 * with an opening the operator could reach into. */
#define SWITCH_DEV       "/dev/input/event0"
#define SW_BIT_DOORS     3
#define SW_BIT_INTERLOCK 5

static int enclosure_open(void)
{
    uint8_t sw[2] = { 0 };
    int fd = open(SWITCH_DEV, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;                       /* cannot read: do not block motion */
    int ok = ioctl(fd, EVIOCGSW(sizeof(sw)), sw) >= 0;
    close(fd);
    if (!ok)
        return 0;
    return (sw[0] & (1u << SW_BIT_DOORS)) || (sw[0] & (1u << SW_BIT_INTERLOCK));
}

/* Head accelerometer: iio device on i2c-3 addr 0x1e (glowforge.dts
 * head-accel). Resolved by bus path, never by iio index - probe order
 * is not guaranteed. */
#define HEAD_ACCEL_GLOB "3-001e"

/* Move geometry. 53.333 microsteps/mm (x8 microstepping, the mode the
 * probe configures). */
#define PROBE_MM        15
#define PROBE_STEPS     (PROBE_MM * 160 / 3)    /* 800 microsteps */
#define STEP_FREQ_HZ    10000
#define TICKS_PER_STEP  19                      /* ~10.5 mm/s */
#define LEAD_TICKS      400

/* Verdict thresholds (bench-characterized 2026-08-11, identical move:
 * dead p2p <= 212, live p2p >= 1040 on X/Y). */
#define P2P_MOVING      500
#define P2P_DEAD        250

/* Pulse-byte bits (kernel feeder contract). */
#define B_X_STEP        0x01
#define B_X_DIR         0x02    /* set = negative X */

static int wr_attr(const char *attr, const char *val)
{
    char path[96];
    snprintf(path, sizeof(path), "/sys/glowforge/%s", attr);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
    close(fd);
    return ret;
}

static int rd_attr(const char *attr, char *buf, size_t len)
{
    char path[96];
    snprintf(path, sizeof(path), "/sys/glowforge/%s", attr);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        n--;
    buf[n] = '\0';
    return 0;
}

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Find the head accel's iio sysfs directory by its i2c bus address. */
static int head_accel_path(char *buf, size_t len)
{
    for (int i = 0; i < 8; i++) {
        char link[128], resolved[256];
        snprintf(link, sizeof(link), "/sys/bus/iio/devices/iio:device%d", i);
        ssize_t n = readlink(link, resolved, sizeof(resolved) - 1);
        if (n < 0)
            continue;
        resolved[n] = '\0';
        if (strstr(resolved, HEAD_ACCEL_GLOB)) {
            snprintf(buf, len, "%s", link);
            return 0;
        }
    }
    return -1;
}

static int accel_read(const char *dir, const char *axis, long *out)
{
    char path[160], buf[24];
    snprintf(path, sizeof(path), "%s/in_accel_%s_raw", dir, axis);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    *out = strtol(buf, NULL, 10);
    return 0;
}

int liveness_probe(int pulse_fd, char *detail, size_t dlen)
{
    char accel[128], st[24];

    if (head_accel_path(accel, sizeof(accel)) != 0) {
        snprintf(detail, dlen, "head accelerometer not found");
        return -1;
    }
    if (rd_attr("cnc/state", st, sizeof(st)) != 0 || strcmp(st, "idle")) {
        snprintf(detail, dlen, "kernel not idle (%s)", st);
        return -1;
    }
    if (enclosure_open()) {
        /* A lid or interlock is open: do not drive the gantry. Report
         * "cannot probe" so the supervisor proceeds without marking a
         * motion fault - the probe re-runs on the next spawn. */
        snprintf(detail, dlen, "door/interlock open - motion probe skipped");
        return -1;
    }

    /* Configure just enough of the machine for a clean X move: x8
     * microstep mode, slow decay, run current on X. The controller
     * re-applies its full config at its own start. */
    wr_attr("cnc/x_mode", "8");
    wr_attr("cnc/x_decay", "1");
    wr_attr("pic/x_step_current", "135");

    char freq[16];
    snprintf(freq, sizeof(freq), "%d", STEP_FREQ_HZ);
    wr_attr("cnc/step_freq", freq);
    lseek(pulse_fd, 1, SEEK_SET);       /* clear program + byte counters */
    wr_attr("cnc/streaming", "0");      /* complete program: EOD = done */

    /* The pattern: lead-in, +X out, pause, -X back, tail. One byte per
     * tick; a set X_STEP bit is one microstep that tick. */
    size_t total = LEAD_TICKS + PROBE_STEPS * TICKS_PER_STEP + LEAD_TICKS
                   + PROBE_STEPS * TICKS_PER_STEP + LEAD_TICKS;
    uint8_t *pat = calloc(1, total);
    if (!pat) {
        snprintf(detail, dlen, "out of memory");
        return -1;
    }
    size_t off = LEAD_TICKS;
    for (int i = 0; i < PROBE_STEPS; i++, off += TICKS_PER_STEP)
        pat[off] = B_X_STEP;                    /* +X: DIR clear */
    off += LEAD_TICKS;
    for (int i = 0; i < PROBE_STEPS; i++, off += TICKS_PER_STEP)
        pat[off] = B_X_STEP | B_X_DIR;          /* back: -X */

    ssize_t wr = write(pulse_fd, pat, total);
    free(pat);
    if (wr != (ssize_t)total) {
        snprintf(detail, dlen, "pulse write failed (%zd of %zu)", wr, total);
        return -1;
    }

    if (wr_attr("cnc/run", "1") != 0) {
        snprintf(detail, dlen, "cannot start the probe run");
        lseek(pulse_fd, 1, SEEK_SET);
        return -1;
    }

    /* Sample the head accel until the run completes (~3.5 s). The raw
     * sysfs reads are slow (~150 ms); that still lands ~10 samples in
     * the window, plenty for a peak-to-peak verdict. */
    long minx = 0, maxx = 0, miny = 0, maxy = 0;
    int have = 0;
    double deadline = wall_s() + 12.0;
    while (wall_s() < deadline) {
        long x, y;
        if (accel_read(accel, "x", &x) == 0 && accel_read(accel, "y", &y) == 0) {
            if (!have) {
                minx = maxx = x;
                miny = maxy = y;
                have = 1;
            } else {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
        if (rd_attr("cnc/state", st, sizeof(st)) == 0 && strcmp(st, "idle") == 0
            && wall_s() > deadline - 10.0)
            break;      /* run finished (state back to idle) */
    }

    /* Stand down: hold current, program cleared. Position counters are
     * net unchanged (equal steps out and back). */
    wr_attr("pic/x_step_current", "33");
    lseek(pulse_fd, 1, SEEK_SET);

    long p2px = maxx - minx, p2py = maxy - miny;
    long p2p = p2px > p2py ? p2px : p2py;
    snprintf(detail, dlen, "head accel p2p x=%ld y=%ld (moving>=%d dead<=%d)",
             p2px, p2py, P2P_MOVING, P2P_DEAD);

    if (!have)
        return -1;
    return p2p >= P2P_MOVING ? 1 : 0;
}

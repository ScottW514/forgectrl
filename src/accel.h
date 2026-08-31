/*
 * accel.h - the head-accelerometer crash watch: two tiers on the
 * LIS2HH12's own interrupt generators
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The factory's head crash detector is the head accelerometer itself:
 * the LIS2HH12 carries two on-chip interrupt generators, and the
 * factory arms them per job from the pulse header's HA* tags. IG1
 * takes a per-axis threshold (the alert tier, a pause); IG2 takes one
 * shared threshold (the abort tier, a fail). Both latch into a
 * per-axis source register the watcher polls and clears by reading.
 * ForgeFIRM adopts the mechanism with local thresholds seeded from a
 * captured cut header: X alert 132, Y alert 112, abort 133, IG
 * register units at the +/-4 g run full scale (LSB = full scale/256,
 * ~15.6 mg). Z is never armed - gravity rides it - and the factory
 * ships Z zero in every captured header too.
 *
 * The generators only sample at a running ODR, and st_accel leaves
 * the part in power-down between one-shot reads, so the armed watch
 * owns CTRL1 (800 Hz) and CTRL4 (+/-4 g) for the window and restores
 * both on disarm; every poll re-asserts them, so a one-shot read
 * elsewhere costs at most a tick. The IG registers themselves are
 * ones st_accel never touches, so the watch runs over i2c-dev with
 * the driver bound and the liveness path untouched.
 *
 * Split like airflow.c: the tier logic is pure (the engine feeds the
 * latched sources, the host test feeds numbers); the i2c access sits
 * behind the crash_hw_* calls.
 */
#ifndef FORGECTRL_ACCEL_H
#define FORGECTRL_ACCEL_H

/* IG_SRC1/IG_SRC2 bits: IA = any latched event, XH/YH the high-event
 * axis sources (the low-event and Z bits are never armed). */
#define CRASH_SRC_IA  0x40
#define CRASH_SRC_XH  0x02
#define CRASH_SRC_YH  0x08

/* A standing alert releases after this many consecutive quiet polls
 * (1 Hz ticks), the fire watch's release shape. */
#define CRASH_CLEAR_TICKS 5

typedef enum {
    CrashEv_None = 0,
    CrashEv_Alert,      /* pause tier newly raised */
    CrashEv_Released,   /* pause tier released after the quiet ticks */
    CrashEv_Alarm       /* fail tier newly latched */
} crash_event_t;

typedef struct {
    int alert;          /* pause tier standing */
    int alarm;          /* fail tier, latched for the run session */
    int clear_ticks;    /* consecutive quiet polls toward release */
    unsigned axes;      /* axis bits of the trip (CRASH_SRC_XH/YH) */
} crash_watch_t;

/* One poll's verdict from the two latched source registers. An abort
 * event latches the alarm for the session; an alert event raises the
 * pause tier and a quiet run of polls releases it. A latched alarm
 * ignores everything after it. */
crash_event_t crash_tick(crash_watch_t *w, unsigned src1, unsigned src2);

/* A new run session: both tiers and the counters cleared. */
void crash_reset(crash_watch_t *w);

/* "x", "y" or "xy" from the recorded axis bits. */
const char *crash_axes_name(unsigned axes);

/* Hardware: the head LIS2HH12 at /dev/i2c-3 addr 0x1e, reached with
 * I2C_SLAVE_FORCE while st_accel stays bound. open probes WHO_AM_I;
 * arm programs the generators (a threshold of 0 leaves that tier
 * unarmed) and takes the ODR and full scale; poll reads and clears
 * both source registers, re-asserting CTRL1/CTRL4 first if a one-shot
 * read elsewhere put the part back to power-down; disarm zeroes the
 * generators and restores every register it took. All return 0 on
 * success, -1 on an i2c failure (open: also when the part does not
 * answer as a LIS2HH12). */
int  crash_hw_open(void);
int  crash_hw_arm(int ths_x_alert, int ths_y_alert, int ths_abort);
int  crash_hw_poll(unsigned *src1, unsigned *src2);
void crash_hw_disarm(void);
void crash_hw_close(void);

#endif

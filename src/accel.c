/*
 * accel.c - forgectrl: head-accelerometer crash watch
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "accel.h"

#include <fcntl.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ---------------------------------------------------- pure tier logic */

crash_event_t crash_tick(crash_watch_t *w, unsigned src1, unsigned src2)
{
    if (w->alarm)
        return CrashEv_None;
    if (src2 & CRASH_SRC_IA) {
        w->alarm = 1;
        w->axes = src2 & (CRASH_SRC_XH | CRASH_SRC_YH);
        return CrashEv_Alarm;
    }
    if (src1 & CRASH_SRC_IA) {
        w->axes = src1 & (CRASH_SRC_XH | CRASH_SRC_YH);
        w->clear_ticks = 0;
        if (!w->alert) {
            w->alert = 1;
            return CrashEv_Alert;
        }
        return CrashEv_None;
    }
    if (w->alert && ++w->clear_ticks >= CRASH_CLEAR_TICKS) {
        w->alert = 0;
        w->clear_ticks = 0;
        return CrashEv_Released;
    }
    return CrashEv_None;
}

void crash_reset(crash_watch_t *w)
{
    w->alert = 0;
    w->alarm = 0;
    w->clear_ticks = 0;
    w->axes = 0;
}

const char *crash_axes_name(unsigned axes)
{
    switch (axes & (CRASH_SRC_XH | CRASH_SRC_YH)) {
    case CRASH_SRC_XH:                return "x";
    case CRASH_SRC_YH:                return "y";
    case CRASH_SRC_XH | CRASH_SRC_YH: return "xy";
    default:                          return "-";
    }
}

/* -------------------------------------------------------------- i2c */

#define BUS_DEV          "/dev/i2c-3"
#define ADDR             0x1e
#define I2C_SLAVE_FORCE  0x0706    /* st_accel stays bound; force the addr */

#define WHO_AM_I         0x0F
#define WHO_AM_I_LIS2HH12 0x41
#define CTRL1            0x20      /* ODR [6:4], BDU, XYZ enables */
#define CTRL4            0x23      /* FS [5:4]: 00 = 2 g, 10 = 4 g */
#define CTRL7            0x26      /* LIR2 bit 3, LIR1 bit 2 */
#define IG_CFG1          0x30
#define IG_SRC1          0x31
#define IG_THS_X1        0x32
#define IG_THS_Y1        0x33
#define IG_THS_Z1        0x34
#define IG_DUR1          0x35
#define IG_CFG2          0x36
#define IG_SRC2          0x37
#define IG_THS2          0x38
#define IG_DUR2          0x39

#define CTRL1_RUN        0x6F      /* 800 Hz, BDU, XYZ on */
#define CTRL4_FS_MASK    0x30
#define CTRL4_FS_4G      0x20      /* the factory run full scale */
#define CTRL7_LIR        0x0C      /* LIR2 | LIR1: latch both sources */
#define CFG_XHIE         0x02
#define CFG_YHIE         0x08

static int i2c_fd = -1;
static unsigned char saved_ctrl1, saved_ctrl4, saved_ctrl7;
static unsigned char run_ctrl4;

static int rd(unsigned char reg, unsigned char *val)
{
    if (write(i2c_fd, &reg, 1) != 1 || read(i2c_fd, val, 1) != 1)
        return -1;
    return 0;
}

static int wr(unsigned char reg, unsigned char val)
{
    unsigned char buf[2] = { reg, val };
    return write(i2c_fd, buf, 2) == 2 ? 0 : -1;
}

int crash_hw_open(void)
{
    unsigned char who;
    if (i2c_fd >= 0)
        return 0;
    i2c_fd = open(BUS_DEV, O_RDWR | O_CLOEXEC);
    if (i2c_fd < 0)
        return -1;
    if (ioctl(i2c_fd, I2C_SLAVE_FORCE, ADDR) < 0 ||
        rd(WHO_AM_I, &who) != 0 || who != WHO_AM_I_LIS2HH12) {
        crash_hw_close();
        return -1;
    }
    return 0;
}

int crash_hw_arm(int ths_x_alert, int ths_y_alert, int ths_abort)
{
    unsigned char cfg1 = 0, cfg2 = 0, src;
    if (i2c_fd < 0)
        return -1;
    if (rd(CTRL1, &saved_ctrl1) || rd(CTRL4, &saved_ctrl4) ||
        rd(CTRL7, &saved_ctrl7))
        return -1;
    run_ctrl4 = (unsigned char)((saved_ctrl4 & ~CTRL4_FS_MASK) | CTRL4_FS_4G);
    if (ths_x_alert > 0)
        cfg1 |= CFG_XHIE;
    if (ths_y_alert > 0)
        cfg1 |= CFG_YHIE;
    if (ths_abort > 0)
        cfg2 |= CFG_XHIE | CFG_YHIE;
    /* Thresholds and latches first, the axis enables last, a stale
     * latch cleared before the first poll judges anything. */
    if (wr(CTRL7, saved_ctrl7 | CTRL7_LIR) ||
        wr(IG_THS_X1, ths_x_alert > 0 ? ths_x_alert & 0xff : 0) ||
        wr(IG_THS_Y1, ths_y_alert > 0 ? ths_y_alert & 0xff : 0) ||
        wr(IG_THS_Z1, 0) || wr(IG_DUR1, 0) ||
        wr(IG_THS2, ths_abort > 0 ? ths_abort & 0xff : 0) ||
        wr(IG_DUR2, 0) ||
        wr(CTRL4, run_ctrl4) || wr(CTRL1, CTRL1_RUN) ||
        wr(IG_CFG1, cfg1) || wr(IG_CFG2, cfg2) ||
        rd(IG_SRC1, &src) || rd(IG_SRC2, &src))
        return -1;
    return 0;
}

int crash_hw_poll(unsigned *src1, unsigned *src2)
{
    unsigned char c1, s1, s2;
    if (i2c_fd < 0)
        return -1;
    /* A one-shot read elsewhere (st_accel) powers the part back down;
     * the generators only sample at a running ODR, so re-assert it.
     * The tick that finds it stomped judges nothing it missed - the
     * window costs at most a poll period. */
    if (rd(CTRL1, &c1))
        return -1;
    if (c1 != CTRL1_RUN && (wr(CTRL4, run_ctrl4) || wr(CTRL1, CTRL1_RUN)))
        return -1;
    if (rd(IG_SRC1, &s1) || rd(IG_SRC2, &s2))
        return -1;
    *src1 = s1;
    *src2 = s2;
    return 0;
}

void crash_hw_disarm(void)
{
    if (i2c_fd < 0)
        return;
    wr(IG_CFG1, 0);
    wr(IG_CFG2, 0);
    wr(CTRL7, saved_ctrl7);
    wr(CTRL4, saved_ctrl4);
    wr(CTRL1, saved_ctrl1);
}

void crash_hw_close(void)
{
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
}

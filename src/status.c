/*
 * status.c - forgectrl: machine operational status
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Gathers the machine's live operational state for the control panel:
 * motion state and position, fan tachometers, coolant temperatures,
 * pump/TEC, laser lockout, and the safety switches. Everything comes
 * from the kernel driver (sysfs + evdev) - the Grbl TCP socket is
 * never touched, because a connection there displaces the sender's
 * session (LightBurn).
 *
 * Position: the controller zeroes the kernel step counters when a
 * homing cycle completes and writes the home coordinates to
 * /run/grblhal.homed, so anchor + counters = machine position. Without
 * the anchor the position is counters-only - relative to wherever the
 * head was when counting started - and flagged unreferenced.
 */
#define _GNU_SOURCE
#include "status.h"
#include "diag.h"

#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GF_SYSFS       "/sys/glowforge/"
#define HOMED_ANCHOR   "/run/grblhal.homed"
#define SWITCH_DEV     "/dev/input/event0"
/* The Glowforge web-service version summary the cloud client (gfcloud /
 * gfhome) records on connect: the latest factory firmware the service
 * advertises and the version this ForgeFIRM release was tested against.
 * Same path as FACTORY_FIRMWARE.STATUS_FILE in gfhome.conf. */
#define GF_LATEST_FILE "/data/forgefirm/gf-latest.json"

/* Kernel step counters -> millimeters (factory-derived constants: 0.15 mm
 * per full step X/Y at the live microstep mode; Z counts half-steps at
 * 0.70612 mm per full step). */
#define XY_MM_PER_FULL_STEP 0.15
#define Z_MM_PER_FULL_STEP  0.70612

static int rd_attr(const char *attr, char *buf, size_t len)
{
    char path[128];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
        n--;
    buf[n] = '\0';
    return 0;
}

static long rd_attr_long(const char *attr, long fallback)
{
    char buf[24];
    if (rd_attr(attr, buf, sizeof(buf)) || !buf[0])
        return fallback;
    return strtol(buf, NULL, 10);
}

/* Factory coolant-thermistor conversion (10k B3380 NTC behind a 10k
 * divider and 1.3 gain, 10-bit ADC) - the curve recovered from the
 * factory firmware and verified against this machine's cloud coolant
 * setpoints and a thermometer. */
double coolant_degc(long raw)
{
    static const double adc_f = 1024.0 * 1.3;
    if (raw <= 0 || (double)raw >= adc_f)
        return -273.15;
    double r = 10000.0 / (adc_f / (double)raw - 1.0);
    double rinf = 10000.0 * exp(-3380.0 / 298.15);
    return 3380.0 / log(r / rinf) - 273.15;
}

/* Tach period -> RPM. The air-assist tach reports a microsecond period
 * at 8 pulses/rev; the chassis fans report nanoseconds at 2 pulses/rev
 * (live-checked: idle air 3900 us -> ~1.9k RPM, intakes ~41 ms).
 * 0 = stalled/stopped. */
static long air_rpm(long period_us)
{
    return period_us > 0 ? (long)(60e6 / ((double)period_us * 8.0)) : 0;
}

static long fan_rpm(long period_ns)
{
    return period_ns > 0 ? (long)(60e9 / ((double)period_ns * 2.0)) : 0;
}

/* Machine position: home anchor + kernel step counters. Without an
 * anchor the machine is unreferenced but still movable - position is
 * then counters-only, i.e. relative to wherever the head was when
 * counting started (the UI paints it red). Returns 0 with xyz and
 * homed filled, or -1 when the counters themselves are unreadable. */
static int read_position(double *x, double *y, double *z, int *homed)
{
    double hx = 0, hy = 0, hz = 0;
    FILE *f = fopen(HOMED_ANCHOR, "r");
    *homed = 0;
    if (f) {
        *homed = fscanf(f, "%lf %lf %lf", &hx, &hy, &hz) == 3;
        fclose(f);
        if (!*homed)
            hx = hy = hz = 0;
    }

    uint8_t raw[32];
    int fd = open(GF_SYSFS "cnc/position", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, raw, sizeof(raw));
    close(fd);
    if (n < 12)
        return -1;

    int32_t sx, sy, sz;
    memcpy(&sx, raw, 4);
    memcpy(&sy, raw + 4, 4);
    memcpy(&sz, raw + 8, 4);

    long xm = rd_attr_long("cnc/x_mode", 8);
    long ym = rd_attr_long("cnc/y_mode", 8);
    if (xm <= 0) xm = 8;
    if (ym <= 0) ym = 8;

    *x = hx + (double)sx / (double)xm * XY_MM_PER_FULL_STEP;
    *y = hy + (double)sy / (double)ym * XY_MM_PER_FULL_STEP;
    *z = hz + (double)sz / 2.0 * Z_MM_PER_FULL_STEP;
    return 0;
}

/* EV_SW bits per the device tree: 0/1 door1/door2, 2 button, 3 doors
 * (combined), 4 estop, 5 interlock, 6 interlock_latch, 7 head.
 * The interlock sense is inverted: the remote-interlock connector (the
 * regulatory 2-pin lockout loop) reads ACTIVE only when the loop is
 * open. Basic/Plus machines ship it factory-jumpered, so the bit stays
 * inactive there = satisfied; Pro brings the connector out for an
 * external lockout chain. */
static unsigned long read_switches(void)
{
    unsigned long bits = 0;
    int fd = open(SWITCH_DEV, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    uint8_t buf[2] = {0};
    if (ioctl(fd, EVIOCGSW(sizeof(buf)), buf) >= 0)
        bits = buf[0] | ((unsigned long)buf[1] << 8);
    close(fd);
    return bits;
}

/* Pull one "key":"value" string out of the small gf-latest.json the
 * web-service client writes. Fills out (empty on absence). */
static void gf_json_str(const char *json, const char *key, char *out, size_t len)
{
    char pat[48];
    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p)
        return;
    p = strchr(p + strlen(pat), ':');
    if (!p)
        return;
    for (p++; *p == ' ' || *p == '\t'; p++)
        ;
    if (*p != '"')
        return;
    size_t i = 0;
    for (p++; *p && *p != '"' && i < len - 1; p++)
        out[i++] = *p;
    out[i] = '\0';
}

/* Read the factory web-service version summary. Fills latest/tested; both
 * empty when the file is absent or unparseable. */
static void read_gf_latest(char *latest, size_t ll, char *tested, size_t tl)
{
    latest[0] = tested[0] = '\0';
    FILE *f = fopen(GF_LATEST_FILE, "r");
    if (!f)
        return;
    char json[256];
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    fclose(f);
    json[n] = '\0';
    gf_json_str(json, "latest_gf_version", latest, ll);
    gf_json_str(json, "tested_against_gf", tested, tl);
}

int machine_is_idle(void)
{
    char st[24];
    if (rd_attr("cnc/state", st, sizeof(st)))
        return 1;   /* no motion driver = nothing can be running */
    return strcmp(st, "idle") == 0;
}

int machine_status_json(char *buf, size_t len)
{
    char state[24] = "";
    rd_attr("cnc/state", state, sizeof(state));

    double x, y, z;
    int homed = 0;
    int have_pos = read_position(&x, &y, &z, &homed) == 0;

    long t1 = rd_attr_long("pic/water_temp_1", -1);
    long t2 = rd_attr_long("pic/water_temp_2", -1);
    long ilk = rd_attr_long("cnc/interlock_circuit", -1);
    unsigned long sw = read_switches();

    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off,
        "{\"state\":\"%s\",\"homed\":%s,\"diag\":%s,",
        state[0] ? state : "unknown", homed ? "true" : "false",
        diag_running() ? "true" : "false");
    if (have_pos)
        off += (size_t)snprintf(buf + off, len - off,
            "\"pos\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},", x, y, z);
    if (ilk >= 0)
        off += (size_t)snprintf(buf + off, len - off,
            "\"laser_locked\":%s,", (ilk & 8) ? "true" : "false");
    off += (size_t)snprintf(buf + off, len - off,
        "\"fans\":{\"air_assist\":%ld,\"exhaust\":%ld,"
        "\"intake_1\":%ld,\"intake_2\":%ld},",
        air_rpm(rd_attr_long("head/air_assist_tach", 0)),
        fan_rpm(rd_attr_long("thermal/tach_exhaust", 0)),
        fan_rpm(rd_attr_long("thermal/tach_intake_1", 0)),
        fan_rpm(rd_attr_long("thermal/tach_intake_2", 0)));
    off += (size_t)snprintf(buf + off, len - off,
        "\"coolant\":{\"down_c\":%.1f,\"up_c\":%.1f,\"pump\":%s,\"tec\":%s},",
        coolant_degc(t1), coolant_degc(t2),
        rd_attr_long("thermal/water_pump_on", 0) ? "true" : "false",
        rd_attr_long("thermal/tec_on", 0) ? "true" : "false");
    char gf_latest[32], gf_tested[32];
    read_gf_latest(gf_latest, sizeof(gf_latest), gf_tested, sizeof(gf_tested));
    if (gf_latest[0] && gf_tested[0])
        off += (size_t)snprintf(buf + off, len - off,
            "\"gfsvc\":{\"latest\":\"%s\",\"tested\":\"%s\"},",
            gf_latest, gf_tested);
    snprintf(buf + off, len - off,
        "\"switches\":{\"lid\":%s,\"button\":%s,\"interlock_ok\":%s,"
        "\"head\":%s,\"estop\":%s}}",
        (sw & (1u << 3)) ? "true" : "false",
        (sw & (1u << 2)) ? "true" : "false",
        (sw & (1u << 5)) ? "false" : "true",
        (sw & (1u << 7)) ? "true" : "false",
        (sw & (1u << 4)) ? "true" : "false");
    return 0;
}

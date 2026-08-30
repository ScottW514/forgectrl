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
#include "super.h"
#include "cool.h"
#include "diag.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Append into a fixed buffer, keeping the running offset within bounds
 * (snprintf returns the would-have-written length, so an unclamped
 * accumulator can underflow the next `size - off`). */
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

/* The sysfs root is fixed in production; GF_SYSFS_ROOT overrides it for
 * host unit tests (the same test-seam idiom as GF_VERDICT_FILE), letting
 * a test point the reader at a temp tree and exercise the fail-closed
 * path with no /sys/glowforge present. Must end with '/'. */
static const char *gf_sysfs_root(void)
{
    static const char *root;
    if (!root) {
        const char *r = getenv("GF_SYSFS_ROOT");
        root = (r && *r) ? r : GF_SYSFS;
    }
    return root;
}

/* Same test seam for the switch device: GF_SWITCH_DEV lets a host test
 * point the reader at a path that is missing or is not an input device,
 * which is how the fail-closed contract of machine_lid_closed() is
 * proven without hardware. */
static const char *switch_dev(void)
{
    static const char *dev;
    if (!dev) {
        const char *d = getenv("GF_SWITCH_DEV");
        dev = (d && *d) ? d : SWITCH_DEV;
    }
    return dev;
}

static int rd_attr(const char *attr, char *buf, size_t len)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s", gf_sysfs_root(), attr);
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
/* The air-assist ground shift on the coolant readings is the cooling
 * engine's to know (cool.c owns the fan); without the engine linked in, a
 * host test that reads status alone takes the raw counts. */
__attribute__((weak)) long cool_coolant_offset_counts(void)
{
    return 0;
}

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
 * (combined), 4 hv_enable (readback of the HV_ENABLE output), 5
 * interlock, 6 interlock_latch, 7 head.
 * The interlock sense is inverted: the remote-interlock connector (the
 * regulatory 2-pin lockout loop) reads ACTIVE only when the loop is
 * open. Basic/Plus machines ship it factory-jumpered, so the bit stays
 * inactive there = satisfied; Pro brings the connector out for an
 * external lockout chain. */
static unsigned long read_switches(void)
{
    unsigned long bits = 0;
    int fd = open(switch_dev(), O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    uint8_t buf[2] = {0};
    if (ioctl(fd, EVIOCGSW(sizeof(buf)), buf) >= 0)
        bits = buf[0] | ((unsigned long)buf[1] << 8);
    close(fd);
    return bits;
}

int machine_lid_closed(void)
{
    /* Bit 3 (`doors`) is the series combination of both lid switches -
     * the same signal the hardware safety chain uses, so it cannot read
     * closed while either switch says otherwise.
     *
     * Fails CLOSED for privacy: read_switches() returns 0 bits on any
     * failure (device missing, not an input device, fd exhaustion), and
     * 0 means "not closed" here, so an unreadable lid keeps the cameras
     * dark rather than letting them capture on a bad read. */
    return (read_switches() & (1u << 3)) != 0;
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
    /* Fail CLOSED: if the state cannot be read - including because the
     * fd table is exhausted by a connection flood (EMFILE) - treat the
     * machine as busy. A read failure must never permit a destructive
     * action (flash, mode switch, diag) or drop safing mid-cut. */
    if (rd_attr("cnc/state", st, sizeof(st)))
        return 0;
    return strcmp(st, "idle") == 0;
}

/* The chassis LM75 by hwmon name, never by index: hwmon numbering
 * follows probe order and hwmon0 is the CPU die on current kernels. The
 * node is resolved once; a board without the sensor resolves to none. */
static const char *lm75_input(void)
{
    static char path[320];
    static int resolved = 0;
    if (resolved)
        return path[0] ? path : NULL;
    resolved = 1;
    DIR *d = opendir("/sys/class/hwmon");
    if (!d)
        return NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "hwmon", 5) != 0)
            continue;
        char np[320], name[32] = "";
        snprintf(np, sizeof(np), "/sys/class/hwmon/%s/name", e->d_name);
        FILE *f = fopen(np, "r");
        if (!f)
            continue;
        if (!fgets(name, sizeof(name), f))
            name[0] = '\0';
        fclose(f);
        if (strncmp(name, "lm75", 4) == 0) {
            snprintf(path, sizeof(path), "/sys/class/hwmon/%s/temp1_input", e->d_name);
            break;
        }
    }
    closedir(d);
    return path[0] ? path : NULL;
}

/* A /sys/class/thermal node by what it says it is, never by number:
 * the zone whose type is the i.MX6 on-die monitor, the cooling device
 * whose type is the CPU frequency scaler. Each resolved once. */
static const char *thermal_node(const char *kind, const char *type_prefix,
                                const char *leaf, char *path, size_t plen,
                                int *resolved)
{
    if (*resolved)
        return path[0] ? path : NULL;
    *resolved = 1;
    DIR *d = opendir("/sys/class/thermal");
    if (!d)
        return NULL;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, kind, strlen(kind)) != 0)
            continue;
        char np[320], type[48] = "";
        snprintf(np, sizeof(np), "/sys/class/thermal/%s/type", e->d_name);
        FILE *f = fopen(np, "r");
        if (!f)
            continue;
        if (!fgets(type, sizeof(type), f))
            type[0] = '\0';
        fclose(f);
        if (strncmp(type, type_prefix, strlen(type_prefix)) == 0) {
            snprintf(path, plen, "/sys/class/thermal/%s/%s", e->d_name, leaf);
            break;
        }
    }
    closedir(d);
    return path[0] ? path : NULL;
}

static long read_long_file(const char *p, long fallback)
{
    if (!p)
        return fallback;
    FILE *f = fopen(p, "r");
    if (!f)
        return fallback;
    long v = fallback;
    if (fscanf(f, "%ld", &v) != 1)
        v = fallback;
    fclose(f);
    return v;
}

double soc_degc(void)
{
    static char path[320];
    static int resolved = 0;
    const char *p = thermal_node("thermal_zone", "imx_thermal_zone", "temp",
                                 path, sizeof(path), &resolved);
    long milli = read_long_file(p, -1);
    return milli >= 0 ? milli / 1000.0 : -273.15;
}

long soc_throttle_state(void)
{
    static char path[320];
    static int resolved = 0;
    const char *p = thermal_node("cooling_device", "cpufreq-cpu", "cur_state",
                                 path, sizeof(path), &resolved);
    return read_long_file(p, -1);
}

double chassis_degc(void)
{
    const char *p = lm75_input();
    if (!p)
        return -273.15;
    FILE *f = fopen(p, "r");
    if (!f)
        return -273.15;
    long milli = 0;
    int ok = fscanf(f, "%ld", &milli) == 1;
    fclose(f);
    return ok ? milli / 1000.0 : -273.15;
}

long supply_temp_raw(void)
{
    return rd_attr_long("pic/pwr_temp", -1);
}

/* CPU utilization from the /proc/stat aggregate line: busy percent over
 * the interval since the previous status read (the panel polls about
 * once a second, so that is the window the number describes). The first
 * read only primes the counters and reports no value. The daemon is
 * thread-per-connection, so the counters sit behind a mutex. Returns
 * -1 when unreadable or unprimed. */
static double cpu_used_pct(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned long long prev_total, prev_idle;
    static int primed;

    FILE *f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    unsigned long long v[8] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
        return -1;
    unsigned long long total = 0;
    for (int i = 0; i < 8; i++)
        total += v[i];
    unsigned long long idle = v[3] + v[4];      /* idle + iowait */

    pthread_mutex_lock(&lock);
    double pct = -1;
    if (primed && total > prev_total && idle >= prev_idle) {
        unsigned long long dt = total - prev_total;
        pct = 100.0 * (double)(dt - (idle - prev_idle)) / (double)dt;
    }
    prev_total = total;
    prev_idle = idle;
    primed = 1;
    pthread_mutex_unlock(&lock);
    return pct;
}

/* Memory utilization: MemTotal against MemAvailable, the kernel's own
 * estimate of what userspace could still claim without swapping.
 * Returns the used percent, or -1 when unreadable. */
static double mem_used_pct(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    long total = -1, avail = -1;
    char line[64];
    while (fgets(line, sizeof(line), f) && (total < 0 || avail < 0)) {
        sscanf(line, "MemTotal: %ld", &total);
        sscanf(line, "MemAvailable: %ld", &avail);
    }
    fclose(f);
    if (total <= 0 || avail < 0 || avail > total)
        return -1;
    return 100.0 * (double)(total - avail) / (double)total;
}

/* The controller's published state file (glowforge_status.c in the
 * driver): one JSON object under the run dir, written atomically with a
 * ts_mono for age. Read whole and validated the way the controller
 * validates the cooling verdict file - a torn body (no closing brace)
 * reads as absent. GF_RUN_DIR overrides the directory for host tests. */
static const char *run_dir(void)
{
    const char *d = getenv("GF_RUN_DIR");
    return d && *d ? d : "/run/forgefirm";
}

static int read_state_file(const char *name, char *buf, size_t len)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", run_dir(), name);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n ? 0 : -1;
}

int grbl_settings_text(char *buf, size_t len)
{
    if (!super_grbl_running())
        return -1;
    return read_state_file("grbl.settings", buf, len);
}

/* Append the controller's grbl.state report with its age, only while
 * the supervisor holds a live GRBL controller: forgectrl supervises the
 * process, so liveness is knowledge, never inference from file age. */
static void append_grbl(char *buf, size_t len, size_t *off)
{
    char body[1024];
    if (!super_grbl_running() || read_state_file("grbl.state", body, sizeof(body)) != 0)
        return;
    char *end = strrchr(body, '}');
    if (body[0] != '{' || !end)
        return;                         /* torn or foreign: absent */
    end[1] = '\0';
    const char *ts = strstr(body, "\"ts_mono\":");
    double age = -1;
    if (ts) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        age = ((double)now.tv_sec + now.tv_nsec / 1e9) - strtod(ts + 10, NULL);
    }
    if (age >= 0 && age < 3600)
        append(buf, len, off, "\"grbl\":{\"age_s\":%.1f,\"report\":%s},", age, body);
}

int machine_status_json(char *buf, size_t len, const char *extra)
{
    char state[24] = "";
    rd_attr("cnc/state", state, sizeof(state));

    double x, y, z;
    int homed = 0;
    int have_pos = read_position(&x, &y, &z, &homed) == 0;

    long t1 = rd_attr_long("pic/water_temp_1", -1);
    long t2 = rd_attr_long("pic/water_temp_2", -1);
    /* The air-assist ground shift lifts the raw counts (more counts read
     * colder); take them off, the same correction the engine applies. */
    long aa_off = cool_coolant_offset_counts();
    if (t1 > aa_off)
        t1 -= aa_off;
    if (t2 > aa_off)
        t2 -= aa_off;
    long ilk = rd_attr_long("cnc/interlock_circuit", -1);
    unsigned long sw = read_switches();

    size_t off = 0;
    append(buf, len, &off,
        "{\"state\":\"%s\",\"homed\":%s,\"diag\":%s,",
        state[0] ? state : "unknown", homed ? "true" : "false",
        diag_running() ? "true" : "false");
    if (have_pos)
        append(buf, len, &off,
            "\"pos\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},", x, y, z);
    /* laser_locked is the COMMANDED latch state (interlock_circuit
     * bit 3 is a driven output read back from the data register - what
     * the SoC last drove, not a sense of the pad). The physical
     * evidence is the sampled block below: emission_samples counts the
     * ~1 s window's samples with the gated LASER_ON output active,
     * pgood_samples counts power-good samples (255 = good all window). */
    if (ilk >= 0)
        append(buf, len, &off,
            "\"laser_locked\":%s,", (ilk & 8) ? "true" : "false");
    long em = rd_attr_long("cnc/laser_on_sampled", -1);
    long pg = rd_attr_long("cnc/laser_pgood_sampled", -1);
    if (em >= 0 && pg >= 0)
        append(buf, len, &off,
            "\"laser\":{\"emission_samples\":%ld,\"pgood_samples\":%ld},",
            em, pg);
    long faults = rd_attr_long("cnc/faults", -1);
    if (faults >= 0)
        append(buf, len, &off, "\"faults\":%ld,", faults);
    long hv = rd_attr_long("pic/hv_current", -1);
    if (hv >= 0)
        append(buf, len, &off, "\"hv_current_raw\":%ld,", hv);
    long ir1 = rd_attr_long("pic/lid_ir_1", -1);
    long ir2 = rd_attr_long("pic/lid_ir_2", -1);
    long ir3 = rd_attr_long("pic/lid_ir_3", -1);
    long ir4 = rd_attr_long("pic/lid_ir_4", -1);
    if (ir1 >= 0 && ir2 >= 0 && ir3 >= 0 && ir4 >= 0)
        append(buf, len, &off,
            "\"lid_ir\":[%ld,%ld,%ld,%ld],", ir1, ir2, ir3, ir4);
    append(buf, len, &off,
        "\"fans\":{\"air_assist\":%ld,\"exhaust\":%ld,"
        "\"intake_1\":%ld,\"intake_2\":%ld},",
        air_rpm(rd_attr_long("head/air_assist_tach", 0)),
        fan_rpm(rd_attr_long("thermal/tach_exhaust", 0)),
        fan_rpm(rd_attr_long("thermal/tach_intake_1", 0)),
        fan_rpm(rd_attr_long("thermal/tach_intake_2", 0)));
    append(buf, len, &off,
        "\"coolant\":{\"down_c\":%.1f,\"up_c\":%.1f,\"pump\":%s,\"tec\":%s},",
        coolant_degc(t1), coolant_degc(t2),
        rd_attr_long("thermal/water_pump_on", 0) ? "true" : "false",
        rd_attr_long("thermal/tec_on", 0) ? "true" : "false");
    /* Watched, not gated: the chassis in degrees, the supply as the raw
     * count its unverified conversion does not earn a unit for. */
    double chassis = chassis_degc();
    long supply = supply_temp_raw();
    append_grbl(buf, len, &off);
    append(buf, len, &off, "\"temps\":{\"chassis_c\":");
    if (chassis > -100)
        append(buf, len, &off, "%.1f", chassis);
    else
        append(buf, len, &off, "null");
    append(buf, len, &off, ",\"supply_raw\":");
    if (supply >= 0)
        append(buf, len, &off, "%ld", supply);
    else
        append(buf, len, &off, "null");
    /* The SoC die and whether the kernel is throttling the CPU for it. */
    double soc = soc_degc();
    long thr = soc_throttle_state();
    append(buf, len, &off, ",\"soc_c\":");
    if (soc > -100)
        append(buf, len, &off, "%.1f", soc);
    else
        append(buf, len, &off, "null");
    append(buf, len, &off, ",\"soc_throttle\":");
    if (thr >= 0)
        append(buf, len, &off, "%ld},", thr);
    else
        append(buf, len, &off, "null},");
    /* SoC load next to its temperature: CPU busy percent over the window
     * since the previous status read, memory used percent by the
     * kernel's MemAvailable estimate. */
    double cpu = cpu_used_pct();
    double mem = mem_used_pct();
    append(buf, len, &off, "\"sys\":{\"cpu_pct\":");
    if (cpu >= 0)
        append(buf, len, &off, "%.1f", cpu);
    else
        append(buf, len, &off, "null");
    append(buf, len, &off, ",\"mem_pct\":");
    if (mem >= 0)
        append(buf, len, &off, "%.1f},", mem);
    else
        append(buf, len, &off, "null},");
    char gf_latest[32], gf_tested[32];
    read_gf_latest(gf_latest, sizeof(gf_latest), gf_tested, sizeof(gf_tested));
    if (gf_latest[0] && gf_tested[0])
        append(buf, len, &off,
            "\"gfsvc\":{\"latest\":\"%s\",\"tested\":\"%s\"},",
            gf_latest, gf_tested);
    /* Head presence is the head driver having probed (its sysfs group
     * exists), never EV_SW bit 7 - that line is not presence (a connected
     * head reads it inactive; it pulses while the head MCU reboots). */
    char hall[16];
    int head_present = rd_attr("head/hall_sensor", hall, sizeof(hall)) == 0;
    if (extra && extra[0])
        append(buf, len, &off, "%s,", extra);
    append(buf, len, &off,
        "\"switches\":{\"lid\":%s,\"button\":%s,\"interlock_ok\":%s,"
        "\"head\":%s,\"hv_enable\":%s}}",
        (sw & (1u << 3)) ? "true" : "false",
        (sw & (1u << 2)) ? "true" : "false",
        (sw & (1u << 5)) ? "false" : "true",
        head_present ? "true" : "false",
        (sw & (1u << 4)) ? "true" : "false");
    return 0;
}

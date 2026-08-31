/*
 * diag.c - forgectrl: hardware diagnostics runner
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics own the hardware while they run: the runner suspends
 * the active controller through the supervisor (launch is idle-gated),
 * drives the thermal loop directly through sysfs - the same model as
 * the bench characterization tools - and resumes the controller on
 * every exit path. A marker file records the ownership so a crash
 * mid-run is recovered at the next forgectrl start (hardware stood
 * down, controller resumed).
 *
 * Cooling tools. The flow check discriminates on how far the
 * downstream water sensor climbs while the loop heater runs: flowing
 * coolant carries the heat away, a stagnant loop cooks the sensor.
 * The bands depend on the pump, the plumbing, and the coolant's
 * thermal properties, so they are per-machine:
 *
 *   flow-verify     one check with the pump on, one with the pump
 *                   commanded off, against the configured threshold.
 *                   PASS = the threshold separates the two readings.
 *   aa-offset-calibrate  the air-assist fan's ground shift on the two
 *                   coolant readings, in ADC counts: the fan stepped
 *                   idle <-> run three times, tube dark, heater off,
 *                   and the step at every edge averaged; recommends
 *                   cool_aa_offset_counts (see cool.c, AA_OFFSET_*).
 *   flow-calibrate  3 trials per case, alternating; reports both
 *                   bands and recommends threshold = band midpoint
 *                   (no recommendation when the gap is too small to
 *                   trust - raise the heater duty instead).
 *
 * Both run at the machine's configured heater duty and window so the
 * verdict applies to the check the cooling engine (cool.c) actually
 * runs; the engine suspends its own writes for the duration.
 */
#define _GNU_SOURCE
#include "cool.h"
#include "diag.h"
#include "fflog.h"
#include "settings.h"
#include "status.h"
#include "super.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GF_SYSFS   "/sys/glowforge/"
#define MARKER     "/run/forgefirm-diag.active"

/* Check parameters come from cool.h - the same compiled defaults the
 * engine runs - and the configured cool_* keys override both, so the
 * tools always test the check the engine actually runs. */
#define DEF_RISE_C     ((double)COOL_FLOW_RISE_C)
#define DEF_HEATER_PCT ((double)COOL_FLOW_HEATER_PCT)
#define DEF_CHECK_S    ((double)COOL_FLOW_CHECK_S)

/* Settle gate (engine-equivalent): baseline capture only from a
 * stationary loop (split-half mean drift over the window) with the
 * sensors in agreement. */
#define SETTLE_WIN       COOL_SETTLE_WIN
#define SETTLE_DRIFT_C   ((double)COOL_SETTLE_DRIFT_C)
#define SETTLE_DT_C      ((double)COOL_SETTLE_DT_C)
#define SETTLE_TIMEOUT_S 420

/* Safety rails for the pump-off trials: the downstream sensor sits at
 * the heater element, so a stagnant window is hard-capped. */
#define ABORT_DOWN_C     48.0

#define CAL_TRIALS       3
/* Below this flow/no-flow gap the midpoint cannot be trusted: the
 * bench matrix put band spreads near +-1.5 C per side. */
#define CAL_MIN_GAP_C    3.0
/* A verify margin thinner than this earns a run-calibration warning. */
#define THIN_MARGIN_C    1.5

/* ------------------------------------------------------------- state */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int st_running = 0;
static int abort_req = 0;
static char st_tool[24];
static char st_phase[80];
static time_t st_started;
static double st_down, st_up;

#define LOG_LINES 48
#define LOG_LEN   96
static char logbuf[LOG_LINES][LOG_LEN];
static int log_n;

static char st_result[768];      /* JSON object, or "" while running */

/* ------------------------------------------------------- hardware io */

static int wr_attr(const char *attr, const char *val)
{
    char path[128];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int ret = write(fd, val, strlen(val)) < 0 ? -1 : 0;
    close(fd);
    return ret;
}

static long rd_long(const char *attr)
{
    char path[128], buf[24];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return strtol(buf, NULL, 10);
}

/* One temperature sample; a glitched read reuses the previous value
 * (a single bad ADC read maps to a wild temperature otherwise). */
static void sample(double *down, double *up)
{
    double d = coolant_degc(rd_long("pic/water_temp_1"));
    double u = coolant_degc(rd_long("pic/water_temp_2"));
    if (d > 5.0 && d < 60.0)
        *down = d;
    if (u > 5.0 && u < 60.0)
        *up = u;
    pthread_mutex_lock(&mu);
    st_down = *down;
    st_up = *up;
    pthread_mutex_unlock(&mu);
}

static void heater_pct(double pct)
{
    char v[16];
    snprintf(v, sizeof(v), "%d", (int)(65535.0 * pct / 100.0));
    wr_attr("thermal/heater_pwm", v);
}

static void pump(int on)
{
    wr_attr("thermal/water_pump_on", on ? "1" : "0");
}

/* Cut-profile chassis fans during a trial - the condition the bands
 * were characterized under and the one the engine's in-job check runs
 * in. Idle values on stand-down; the engine reapplies its own fan
 * policy when it takes back over. */
static void fans_run(void)
{
    char v[16];
    snprintf(v, sizeof(v), "%d", COOL_EXHAUST_RUN);
    wr_attr("thermal/exhaust_pwm", v);
    snprintf(v, sizeof(v), "%d", COOL_INTAKE_RUN);
    wr_attr("thermal/intake_pwm", v);
}

static void fans_idle(void)
{
    wr_attr("thermal/exhaust_pwm", "0");
    wr_attr("thermal/intake_pwm", "0");
}

static void stand_down(void)
{
    heater_pct(0);
    pump(1);
    fans_idle();
}

/* ------------------------------------------------- controller service */

/* The supervisor owns the controller lifecycle: suspend takes the
 * hardware (the active mode's controller stops), resume gives it back
 * (the selected mode's controller returns - whichever that is). */
static int controller_stop(void)
{
    return super_controller_stop();
}

static void controller_start(void)
{
    super_controller_start();
}

/* ------------------------------------------------------ log + status */

static void dlog(const char *fmt, ...)
{
    char line[LOG_LEN - 16];    /* room for the elapsed-time prefix */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&mu);
    long el = (long)(time(NULL) - st_started);
    if (el < 0)
        el = 0;
    if (el > 5999)
        el = 5999;                  /* display cap 99:59 */
    snprintf(logbuf[log_n % LOG_LINES], LOG_LEN, "%3ld:%02ld  %s",
             el / 60, el % 60, line);
    log_n++;
    pthread_mutex_unlock(&mu);
    fflog(LOG_INFO, "diag %s", line);
}

static void set_phase(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&mu);
    vsnprintf(st_phase, sizeof(st_phase), fmt, ap);
    pthread_mutex_unlock(&mu);
    va_end(ap);
}

static void set_result(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&mu);
    vsnprintf(st_result, sizeof(st_result), fmt, ap);
    pthread_mutex_unlock(&mu);
    va_end(ap);
}

/* ---------------------------------------------------------- settling */

static double cfg_num(const char *key, double def)
{
    char v[32];
    char *end;
    if (settings_get(key, v, sizeof(v)) != 0)
        return def;
    double f = strtod(v, &end);
    return end == v ? def : f;
}

static int aborted(void)
{
    pthread_mutex_lock(&mu);
    int a = abort_req;
    pthread_mutex_unlock(&mu);
    return a;
}

/* Wait for a stationary loop (driver-equivalent gate). Returns 0, or
 * -1 on timeout, -2 on abort. */
static int settle(double *down, double *up)
{
    double win[SETTLE_WIN];
    int n = 0;

    for (int t = 0; t < SETTLE_TIMEOUT_S; t++) {
        if (aborted())
            return -2;
        sample(down, up);
        win[n % SETTLE_WIN] = *down;
        n++;
        if (n >= SETTLE_WIN) {
            double old_s = 0, new_s = 0;
            int base = n - SETTLE_WIN;
            for (int i = 0; i < 7; i++)
                old_s += win[(base + i) % SETTLE_WIN];
            for (int i = 7; i < SETTLE_WIN; i++)
                new_s += win[(base + i) % SETTLE_WIN];
            double drift = new_s / 8.0 - old_s / 7.0;
            if (drift < 0)
                drift = -drift;
            if (drift <= SETTLE_DRIFT_C &&
                (*down > *up ? *down - *up : *up - *down) <= SETTLE_DT_C)
                return 0;
        }
        sleep(1);
    }
    return -1;
}

/* One heater trial. Returns 0 with rise/dt filled, -1 on abort, -2 on
 * the safety ceiling. The pump is always restored. */
static int trial(int flow, double duty, int secs,
                 double *rise, double *dt)
{
    double down = 20, up = 20;

    pump(flow);
    fans_run();
    sleep(3);
    sample(&down, &up);
    double base = down;

    heater_pct(duty);
    int rc = 0;
    for (int t = 0; t < secs; t++) {
        if (aborted()) {
            rc = -1;
            break;
        }
        sample(&down, &up);
        if (down >= ABORT_DOWN_C) {
            dlog("safety: downstream %.1f C - trial aborted", down);
            rc = -2;
            break;
        }
        sleep(1);
    }
    heater_pct(0);
    pump(1);

    *rise = down - base;
    *dt = down - up;
    return rc;
}

/* ------------------------------------------------------------ runner */

static void *runner(void *arg)
{
    int calibrate = arg != NULL;
    double thr = cfg_num("cool_flow_rise", DEF_RISE_C);
    double duty = cfg_num("cool_flow_heater_pct", DEF_HEATER_PCT);
    int secs = (int)cfg_num("cool_flow_check_s", DEF_CHECK_S);
    if (secs <= 0)
        secs = (int)DEF_CHECK_S;    /* checks disabled: test the default */

    dlog("start: duty %.0f%%, window %d s, threshold %.1f C",
         duty, secs, thr);

    set_phase("stopping the motion controller");
    if (controller_stop()) {
        set_result("{\"error\":\"could not stop the motion controller\"}");
        /* The stop already un-suspended on timeout; this additionally
         * clears any motion fault and resets the respawn backoff, so a
         * failed takeover can never leave the machine controller-less. */
        controller_start();
        goto out_norestart;
    }
    close(open(MARKER, O_CREAT | O_WRONLY, 0644));

    double f_rise[CAL_TRIALS], f_dt[CAL_TRIALS];
    double n_rise[CAL_TRIALS], n_dt[CAL_TRIALS];
    int trials = calibrate ? CAL_TRIALS : 1;

    for (int t = 0; t < trials; t++) {
        for (int flow = 1; flow >= 0; flow--) {
            double down, up;
            set_phase("settling before trial %d/%d (%s)",
                      t + 1, trials, flow ? "pump on" : "pump off");
            int rc = settle(&down, &up);
            if (rc == -1) {
                set_result("{\"error\":\"loop would not settle within "
                           "%d s\"}", SETTLE_TIMEOUT_S);
                goto out;
            }
            if (rc == -2)
                goto out_aborted;

            set_phase("trial %d/%d: heater %.0f%% for %d s (%s)",
                      t + 1, trials, duty, secs,
                      flow ? "pump on" : "pump off");
            double *rise = flow ? &f_rise[t] : &n_rise[t];
            double *dt = flow ? &f_dt[t] : &n_dt[t];
            rc = trial(flow, duty, secs, rise, dt);
            if (rc == -1)
                goto out_aborted;
            if (rc == -2) {
                set_result("{\"error\":\"downstream sensor hit %.0f C - "
                           "aborted for safety\"}", ABORT_DOWN_C);
                goto out;
            }
            dlog("trial %d/%d %s: rise %.1f C, dT %.1f C", t + 1, trials,
                 flow ? "flow" : "no-flow", *rise, *dt);
        }
    }

    if (!calibrate) {
        int pass = f_rise[0] < thr && thr < n_rise[0];
        double m_flow = thr - f_rise[0], m_noflow = n_rise[0] - thr;
        int thin = m_flow < THIN_MARGIN_C || m_noflow < THIN_MARGIN_C;
        set_result("{\"pass\":%s,\"threshold\":%.1f,"
                   "\"flow_rise\":%.1f,\"flow_dt\":%.1f,"
                   "\"noflow_rise\":%.1f,\"noflow_dt\":%.1f,"
                   "\"margin_flow\":%.1f,\"margin_noflow\":%.1f,"
                   "\"thin_margin\":%s}",
                   pass ? "true" : "false", thr,
                   f_rise[0], f_dt[0], n_rise[0], n_dt[0],
                   m_flow, m_noflow,
                   (pass && thin) ? "true" : "false");
        dlog("verdict: %s (flow %.1f / threshold %.1f / no-flow %.1f)",
             pass ? "PASS" : "FAIL", f_rise[0], thr, n_rise[0]);
    } else {
        double f_max = f_rise[0], n_min = n_rise[0];
        for (int t = 1; t < CAL_TRIALS; t++) {
            if (f_rise[t] > f_max)
                f_max = f_rise[t];
            if (n_rise[t] < n_min)
                n_min = n_rise[t];
        }
        double gap = n_min - f_max;
        if (gap < CAL_MIN_GAP_C)
            set_result("{\"flow_rises\":[%.1f,%.1f,%.1f],"
                       "\"noflow_rises\":[%.1f,%.1f,%.1f],"
                       "\"flow_max\":%.1f,\"noflow_min\":%.1f,"
                       "\"gap\":%.1f,"
                       "\"error\":\"bands too close to separate at this "
                       "duty - raise cool_flow_heater_pct and rerun\"}",
                       f_rise[0], f_rise[1], f_rise[2],
                       n_rise[0], n_rise[1], n_rise[2],
                       f_max, n_min, gap);
        else
            set_result("{\"flow_rises\":[%.1f,%.1f,%.1f],"
                       "\"noflow_rises\":[%.1f,%.1f,%.1f],"
                       "\"flow_max\":%.1f,\"noflow_min\":%.1f,"
                       "\"gap\":%.1f,\"recommend\":%.1f}",
                       f_rise[0], f_rise[1], f_rise[2],
                       n_rise[0], n_rise[1], n_rise[2],
                       f_max, n_min, gap, (f_max + n_min) / 2.0);
        dlog("bands: flow <= %.1f, no-flow >= %.1f (gap %.1f)",
             f_max, n_min, gap);
    }
    goto out;

out_aborted:
    set_result("{\"error\":\"aborted by operator\"}");
out:
    set_phase("standing down");
    stand_down();
    controller_start();
    unlink(MARKER);
out_norestart:
    dlog("done");
    set_phase("done");
    pthread_mutex_lock(&mu);
    st_running = 0;
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* ------------------------------------------- aa-offset-calibrate */

/* The air-assist fan's return current shifts both coolant thermistor
 * readings by a voltage the ADC sees as a constant number of counts
 * while the fan runs (cool.c, AA_OFFSET_*). This tool measures that
 * number on this machine: pump on, heater off, tube dark, the air assist
 * stepped idle -> run -> idle AA_CYCLES times with both sensors read at
 * AA_HZ, the step at every edge taken as the mean of the 1.5 s after the
 * edge (from 0.3 s) minus the mean of the 1.5 s before, in counts, on
 * each sensor; the recommendation is the mean of the edges' magnitudes,
 * and the spread across them is reported so a noisy loop refuses rather
 * than recommends. */
/* The single-sample noise on these sensors is about 5 counts, so an
 * edge read as the difference of two short means needs long ones: 3 s
 * at 8 Hz is 24 samples a side, about 1 count of noise on each mean.
 * (1.5 s at 4 Hz read 10 to 21 counts across six edges of a 16-count
 * step, and refused its own result.) */
#define AA_CYCLES     3
#define AA_HZ         8
#define AA_WIN_S      3.0
#define AA_DWELL_S    10
#define AA_IDLE       204
#define AA_RUN        1023
#define AA_EDGES      (AA_CYCLES * 2)
#define AA_MAX_SPREAD 8.0
#define AA_MIN_COUNTS 3.0

static void aa_write(long duty)
{
    char v[16];
    snprintf(v, sizeof(v), "%ld", duty);
    wr_attr("head/air_assist_pwm", v);
}

/* One edge: settle the fan's new state and read the step on both
 * sensors. Returns 0, -1 on abort. */
static int aa_edge(long duty, double *step_down, double *step_up)
{
    enum { N = (int)(AA_WIN_S * AA_HZ) };
    double b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    int nb = 0, na = 0;
    for (int i = 0; i < N; i++) {
        long r1 = rd_long("pic/water_temp_1"), r2 = rd_long("pic/water_temp_2");
        if (r1 > 0 && r2 > 0) {
            b1 += (double)r1;
            b2 += (double)r2;
            nb++;
        }
        usleep(1000000 / AA_HZ);
        if (aborted())
            return -1;
    }
    aa_write(duty);
    usleep(500000);                     /* the fan's current settles */
    for (int i = 0; i < N; i++) {
        long r1 = rd_long("pic/water_temp_1"), r2 = rd_long("pic/water_temp_2");
        if (r1 > 0 && r2 > 0) {
            a1 += (double)r1;
            a2 += (double)r2;
            na++;
        }
        usleep(1000000 / AA_HZ);
        if (aborted())
            return -1;
    }
    if (!nb || !na)
        return -1;
    *step_down = a1 / na - b1 / nb;
    *step_up = a2 / na - b2 / nb;
    /* The rest of the dwell, so the loop is steady before the next edge. */
    for (int i = 0; i < AA_DWELL_S - (int)AA_WIN_S - 1; i++) {
        double d, u;
        sample(&d, &u);
        sleep(1);
        if (aborted())
            return -1;
    }
    return 0;
}

static void *runner_aa(void *arg)
{
    (void)arg;
    dlog("start: air assist %d <-> %d, %d cycles", AA_IDLE, AA_RUN, AA_CYCLES);
    set_phase("stopping the motion controller");
    if (controller_stop()) {
        set_result("{\"error\":\"could not stop the motion controller\"}");
        controller_start();
        goto out_norestart;
    }
    close(open(MARKER, O_CREAT | O_WRONLY, 0644));

    double sd[AA_EDGES], su[AA_EDGES];
    int n = 0;
    pump(1);
    heater_pct(0);
    fans_idle();
    aa_write(AA_IDLE);
    /* The edges are read against a trend-free baseline: the same
     * stationary-loop gate the flow tools use (drift and down/up
     * agreement), so a loop still mixing after a heater trial waits
     * here instead of putting its trend into the first edge. */
    set_phase("settling with the fans idle");
    {
        double d, u;
        int rc = settle(&d, &u);
        if (rc == -1) {
            set_result("{\"error\":\"loop would not settle within %d s\"}",
                       SETTLE_TIMEOUT_S);
            goto out;
        }
        if (rc == -2)
            goto out_aborted;
    }
    for (int c = 0; c < AA_CYCLES; c++) {
        set_phase("cycle %d/%d: air assist to run", c + 1, AA_CYCLES);
        if (aa_edge(AA_RUN, &sd[n], &su[n]))
            goto out_aborted;
        dlog("edge %d (to run): down %+.1f, up %+.1f counts", n + 1, sd[n], su[n]);
        n++;
        set_phase("cycle %d/%d: air assist to idle", c + 1, AA_CYCLES);
        if (aa_edge(AA_IDLE, &sd[n], &su[n]))
            goto out_aborted;
        dlog("edge %d (to idle): down %+.1f, up %+.1f counts", n + 1, sd[n], su[n]);
        n++;
    }
    {
        double sum = 0, lo = 1e9, hi = -1e9;
        for (int i = 0; i < n; i++) {
            double m1 = sd[i] < 0 ? -sd[i] : sd[i];
            double m2 = su[i] < 0 ? -su[i] : su[i];
            sum += m1 + m2;
            if (m1 < lo) lo = m1;
            if (m2 < lo) lo = m2;
            if (m1 > hi) hi = m1;
            if (m2 > hi) hi = m2;
        }
        double counts = sum / (2.0 * n), spread = hi - lo;
        char steps[256];
        int off = 0;
        for (int i = 0; i < n && off < (int)sizeof(steps) - 24; i++)
            off += snprintf(steps + off, sizeof(steps) - off, "%s[%.1f,%.1f]",
                            i ? "," : "", sd[i], su[i]);
        if (spread > AA_MAX_SPREAD)
            set_result("{\"steps\":[%s],\"offset_counts\":%.1f,\"spread_counts\":%.1f,"
                       "\"error\":\"edges disagree by %.1f counts - the loop is not "
                       "quiet, or a fan did not follow its duty; rerun\"}",
                       steps, counts, spread, spread);
        else
            set_result("{\"steps\":[%s],\"offset_counts\":%.1f,\"spread_counts\":%.1f,"
                       "\"recommend\":%.1f}",
                       steps, counts, spread, counts < AA_MIN_COUNTS ? 0.0 : counts);
        dlog("offset %.1f counts (spread %.1f) over %d edges", counts, spread, n);
    }
    goto out;

out_aborted:
    set_result("{\"error\":\"aborted by operator\"}");
out:
    set_phase("standing down");
    aa_write(AA_IDLE);
    stand_down();
    controller_start();
    unlink(MARKER);
out_norestart:
    dlog("done");
    set_phase("done");
    pthread_mutex_lock(&mu);
    st_running = 0;
    pthread_mutex_unlock(&mu);
    return NULL;
}

/* --------------------------------------------------------------- api */

void diag_init(void)
{
    if (access(MARKER, F_OK) == 0) {
        fflog(LOG_WARNING, "stale diagnostic marker - standing "
                           "down and restarting the controller");
        stand_down();
        controller_start();
        unlink(MARKER);
    }
}

int diag_running(void)
{
    pthread_mutex_lock(&mu);
    int r = st_running;
    pthread_mutex_unlock(&mu);
    return r;
}

int diag_start(const char *tool)
{
    int calibrate;
    void *(*fn)(void *) = runner;
    if (!strcmp(tool, "flow-verify"))
        calibrate = 0;
    else if (!strcmp(tool, "flow-calibrate"))
        calibrate = 1;
    else if (!strcmp(tool, "aa-offset-calibrate")) {
        calibrate = 0;
        fn = runner_aa;
    } else
        return -3;

    pthread_mutex_lock(&mu);
    if (st_running) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    if (!machine_is_idle()) {
        pthread_mutex_unlock(&mu);
        return -2;
    }
    st_running = 1;
    abort_req = 0;
    log_n = 0;
    st_result[0] = '\0';
    st_phase[0] = '\0';
    st_started = time(NULL);
    snprintf(st_tool, sizeof(st_tool), "%s", tool);
    pthread_mutex_unlock(&mu);

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, fn,
                       calibrate ? (void *)1 : NULL) != 0) {
        pthread_mutex_lock(&mu);
        st_running = 0;
        pthread_mutex_unlock(&mu);
        pthread_attr_destroy(&at);
        return -1;
    }
    pthread_attr_destroy(&at);
    return 0;
}

void diag_abort(void)
{
    pthread_mutex_lock(&mu);
    if (st_running)
        abort_req = 1;
    pthread_mutex_unlock(&mu);
}

int diag_status_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    size_t off = 0;
    off += (size_t)snprintf(buf + off, len - off,
        "{\"running\":%s,\"tool\":\"%s\",\"phase\":\"%s\","
        "\"elapsed_s\":%ld,\"down_c\":%.1f,\"up_c\":%.1f,\"log\":[",
        st_running ? "true" : "false", st_tool, st_phase,
        st_running ? (long)(time(NULL) - st_started) : 0,
        st_down, st_up);
    int first = log_n > LOG_LINES ? log_n - LOG_LINES : 0;
    for (int i = first; i < log_n && off < len - 8; i++) {
        off += (size_t)snprintf(buf + off, len - off, "%s\"%s\"",
                                i > first ? "," : "",
                                logbuf[i % LOG_LINES]);
        if (off >= len)             /* a long line overshot: stop appending */
            off = len - 1;
    }
    snprintf(buf + off, len - off, "],\"result\":%s}",
             st_result[0] ? st_result : "null");
    pthread_mutex_unlock(&mu);
    return 0;
}

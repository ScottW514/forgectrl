/*
 * diag.c - forgectrl: hardware diagnostics runner
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics own the hardware while they run: the runner stops the
 * grblhal service (launch is idle-gated), drives the thermal loop
 * directly through sysfs - the same model as the bench
 * characterization tools - and restarts the service on every exit
 * path. A marker file records the ownership so a crash mid-run is
 * recovered at the next forgectrl start (hardware stood down,
 * controller restarted).
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
#include "settings.h"
#include "status.h"

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
#define INIT_GRBL  "/etc/init.d/grblhal"

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

static int controller_running(void)
{
    return system("pgrep grblHAL_glowfor >/dev/null 2>&1") == 0;
}

static int controller_stop(void)
{
    system(INIT_GRBL " stop >/dev/null 2>&1");
    for (int i = 0; i < 20 && controller_running(); i++)
        usleep(250 * 1000);
    return controller_running() ? -1 : 0;
}

/* The init script gates itself on controller_mode and its pidfile, so
 * an unconditional start is always safe. */
static void controller_start(void)
{
    system(INIT_GRBL " start >/dev/null 2>&1");
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
    snprintf(logbuf[log_n % LOG_LINES], LOG_LEN, "%3ld:%02ld  %s",
             el / 60, el % 60, line);
    log_n++;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "forgectrl: diag %s\n", line);
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

/* --------------------------------------------------------------- api */

void diag_init(void)
{
    if (access(MARKER, F_OK) == 0) {
        fprintf(stderr, "forgectrl: stale diagnostic marker - standing "
                        "down and restarting the controller\n");
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
    if (!strcmp(tool, "flow-verify"))
        calibrate = 0;
    else if (!strcmp(tool, "flow-calibrate"))
        calibrate = 1;
    else
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
    if (pthread_create(&th, &at, runner,
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
    for (int i = first; i < log_n && off < len - 8; i++)
        off += (size_t)snprintf(buf + off, len - off, "%s\"%s\"",
                                i > first ? "," : "",
                                logbuf[i % LOG_LINES]);
    snprintf(buf + off, len - off, "],\"result\":%s}",
             st_result[0] ? st_result : "null");
    pthread_mutex_unlock(&mu);
    return 0;
}

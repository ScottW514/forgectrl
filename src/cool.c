/*
 * cool.c - forgectrl: the cooling engine
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Single owner of the thermal hardware (fans, pump, TEC, loop heater)
 * for every controller mode, per docs/SERVICES.md. Job state arrives
 * as level-triggered reports (POST /cool/state, ~1 Hz); the engine
 * publishes its verdict to /run/forgefirm/cooling.state (atomic
 * replace, ~1 Hz). Enforcement - the laser fire gate and feed-hold
 * issuance - stays in-process with the active controller; a reader
 * that finds the verdict file missing or stale treats it as
 * fire-blocked. The hardware AND-gate remains the safety boundary;
 * everything here is equipment protection.
 *
 * Policy, built from the factory's own numbers (captured pulse-file
 * headers + settings map): fan duties AAid=204/AArd=1023, EFrd=65535,
 * IFrd=43278; coolant windows CM* in millidegrees - run ceiling 33 C
 * (CMrx), start/resume gate 31 C (CMwx). All thresholds user-tunable:
 * cool_* keys in the shared machine settings (re-read at every run
 * start), GFCOOL_* env vars as bench overrides that win for the
 * process lifetime.
 *
 * - IDLE: pump on, TEC off, purge on, fans at factory idle, HEATER OFF
 *   (an always-on flow heater measurably warms the loop - ~1.5 C in
 *   minutes - eating headroom below the start gate for no benefit
 *   while nothing can fire).
 * - RUN (reported mode "run", or armed whatever the mode says):
 *   cut-profile fans - per-job duties from the report when given -
 *   plus periodic coolant-flow verification. The flow heater sits
 *   between the two water-temp sensors; each check runs it at
 *   FLOW_HEATER_PCT for FLOW_CHECK_S and reads how far the downstream
 *   sensor climbs (flowing coolant carries the heat away; a stagnant
 *   loop cooks the downstream sensor). Checks repeat every
 *   FLOW_RECHECK_S and start only from a thermally settled loop. An
 *   over-limit reading is a SUSPICION, not a fault: it publishes a
 *   hold request and re-checks immediately, and only a second
 *   consecutive over-limit (no clean check in between) - or a
 *   suspicion unresolved within FLOW_CONFIRM_MAX_S - raises COOLANT
 *   FLOW FAULT. A clean re-check clears the suspicion: transients (a
 *   pump airlock burp, disturbed coolant) self-clear within minutes,
 *   while true stagnation cannot pass a settled re-check. Cleared
 *   suspicions still count - FLOW_TREND_N of them in one job earn an
 *   aggregated check-your-coolant warning.
 * - COOLDOWN (run ends): a smoke-clear phase at run duty, then a
 *   thermal phase at reduced duty (fan airflow measurably cools the
 *   loop) until the upstream temp is back under the resume gate or a
 *   timeout expires; heater off throughout (it would fight the
 *   cooling).
 * - OVER-TEMP: if the upstream temp exceeds the run ceiling the
 *   verdict goes OVERTEMP with hold=true and cooling airflow forced;
 *   it recovers (resume_ok) once the temp is back under the resume
 *   gate. The upstream sensor gates because it reads the coolant
 *   entering the tube.
 * - Controller silence: if the active controller stops reporting past
 *   REPORT_TIMEOUT_S, fire_ok goes false immediately and the engine
 *   stands down through the normal cooldown path (smoke clear is the
 *   right physical behavior for a job that died mid-cut).
 * - Diagnostics (diag.c) own the hardware while they run: the engine
 *   suspends its writes and publishes fire-blocked until they finish.
 */
#define _GNU_SOURCE
#include "cool.h"
#include "diag.h"
#include "settings.h"
#include "status.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define GF_SYSFS     "/sys/glowforge/"
#define VERDICT_DIR  "/run/forgefirm"
#define VERDICT_FILE VERDICT_DIR "/cooling.state"
#define VERDICT_TMP  VERDICT_DIR "/.cooling.state.tmp"

/* A controller reports at ~1 Hz; past this silence it is treated as
 * gone (fire blocked, stand-down through cooldown). */
#define REPORT_TIMEOUT_S 5.0

/* Factory fan values (pulse-header ground truth; run duties shared via
 * cool.h). */
#define AIR_ASSIST_IDLE 204
#define AIR_ASSIST_RUN  COOL_AIR_ASSIST_RUN
#define EXHAUST_IDLE    0
#define EXHAUST_RUN     COOL_EXHAUST_RUN
#define INTAKE_IDLE     0
#define INTAKE_RUN      COOL_INTAKE_RUN
/* Thermal-cooldown duty: half the run airflow - enough to keep pulling
 * heat out of the loop without the full-run roar. */
#define EXHAUST_COOL    32768
#define INTAKE_COOL     21639

/* Defaults, all tunable (see the table below). Temperature values are
 * the factory coolant-monitor windows (job-header CM*, millidegrees):
 * run ceiling CMrx=33 C, start/resume gate CMwx=31 C. */
#define COOLDOWN_SMOKE_S       15
#define COOLDOWN_MAX_S         300
#define TEMP_MAX_C_DEFAULT     33.0f
#define TEMP_RESUME_C_DEFAULT  31.0f

/* Flow check. Characterized on the machine with the factory
 * temperature curve (forgefirm scripts/bench/flow_characterize.py):
 *   heater 10% -> flow dT <= +3.69, no-flow dT >= +3.74  (0.04 C gap:
 *                 UNUSABLE, sensor noise alone spans ~0.9 C)
 *   heater 30% -> flow dT <= +9.32, no-flow dT >= +10.99 (1.67 C gap)
 * So the check runs hotter and only briefly: the delta plateaus by
 * ~30 s, and continuous heating would keep warming the loop. After
 * the check the heater goes off and absolute-temperature monitoring
 * carries the protection (a pump failure mid-cut shows up far faster
 * as a temperature climb than as a heater delta).
 *
 * Duty matters more than anything else here. Below ~40% the stagnant
 * loop sheds the heater's output by natural convection well enough to
 * MIMIC FLOW: at 30%/50 s the five no-flow trials read 8.15, 8.69,
 * 8.78, 12.25, 13.33 C while flow never exceeded 9.08 - three of five
 * dead-pump cases look healthier than a working pump. At 40% the heat
 * input outruns convection and the signature becomes decisive and
 * repeatable (design matrix: forgefirm scripts/bench/flow_matrix.py). */
#define FLOW_HEATER_PCT    COOL_FLOW_HEATER_PCT
#define FLOW_CHECK_S       COOL_FLOW_CHECK_S    /* 0 disables the check */
#define FLOW_ESTABLISH_S   30      /* delta plateaus by here (reporting only) */
/* Re-check cadence while a job runs. A stopped pump CANNOT be seen any
 * other way: absolute temperature only tracks a circulating loop, and a
 * light engrave may add so little heat that a flat trend proves
 * nothing. Detection latency is this interval plus the check length. */
#define FLOW_RECHECK_S     150

/* A check is only meaningful from a thermally SETTLED loop. If the
 * downstream sensor is still falling from earlier heating, the baseline
 * is captured mid-transient and the measured rise is garbage -
 * bench-proven to misclassify in BOTH directions, including reporting
 * flow when the pump was stopped (a MISS).
 *
 * Sensor agreement alone is NOT sufficient: both sensors can be
 * plunging together and cross within any tolerance while the loop is
 * still far from steady - that exact case produced a miss on the bench
 * (rise 11.2 with the pump stopped, from a loop cooling out of 48 C).
 * So the gate also requires the downstream reading to be STATIONARY
 * over a window before the baseline is taken.
 *
 * Stationarity is measured as the difference between the mean of the
 * newer half of the window and the mean of the older half. Peak-to-peak
 * does not work: measured on a settled loop this sensor shows 0.52 C of
 * p-p jitter over 15 s (0.70 worst), so any p-p threshold tight enough
 * to catch drift sits below the noise floor and the gate never opens.
 * Averaging each half cuts that to 0.11 C typical / 0.21 C worst, while
 * a real cooling transient (~2 C per 15 s) still shows ~1.5 C. */
#define FLOW_SETTLE_DT_C    COOL_SETTLE_DT_C
#define FLOW_SETTLE_DRIFT_C COOL_SETTLE_DRIFT_C
#define FLOW_SETTLE_WIN     COOL_SETTLE_WIN     /* 1 Hz samples */
#define FLOW_SETTLE_WARN_S  180

/* The DISCRIMINATOR is how far the downstream sensor climbs during the
 * check, not the upstream/downstream delta. Measured from cold at 30%
 * heater: with flow the downstream plateaus at about +10.5 C, without
 * flow it passes +16 C and is still climbing - a ~6 C margin, versus
 * only ~2 C for the delta. The delta is NOT a usable discriminator: a
 * check that starts from a cold heater never reaches the steady-state
 * delta (bench: a live pump-off drill reads 8.8 C against a 10.2 C
 * limit - a false negative).
 *
 * Balanced midpoint of the pooled bracket at 40%/50 s: across 17 flow
 * observations (design matrix, repeat validations, and a 40-minute
 * sustained run) the largest healthy rise was 12.75 C, and across 8
 * pump-stopped observations the smallest was 16.04 C. 14.4 sits ~1.6 C
 * from either edge. All baselines were 19-23 C; the warm-loop end of
 * the range is NOT yet validated. GFCOOL_FLOW_RISE overrides. */
#define FLOW_FAULT_RISE_C  COOL_FLOW_RISE_C

/* A suspicion must resolve. With flow, the check's own heat sheds in
 * under a minute and the confirming verdict lands 2-4 min after the
 * suspect one; with a truly dead pump the cooked region needs ~3-5 min
 * of conduction-only decay before the settle gate reopens, and the
 * re-check then reads stagnant again. A loop that cannot produce any
 * verdict inside this budget has shown no evidence of health and is
 * treated as faulted. The budget restarts with each run session -
 * checks cannot run outside one. GFCOOL_CONFIRM_MAX_S overrides. */
#define FLOW_CONFIRM_MAX_S 480
/* Cleared suspicions per job that earn an aggregated warning: each one
 * was disproven by its re-check, but several in a single job point at
 * a marginal pump or recurring airlock. */
#define FLOW_TREND_N       3

typedef enum {
    Cool_Idle = 0,
    Cool_Run,
    Cool_Smoke,      /* post-job smoke clear at run duty */
    Cool_Thermal,    /* reduced-duty airflow until temp recovers */
} cool_state_t;

/* One over-limit reading opens a suspicion; the next completed check
 * decides it, whenever that is - "consecutive" means no clean check in
 * between, not close in time. */
typedef enum {
    Flow_Normal = 0,
    Flow_Suspect,       /* one over-limit; the re-check decides */
    Flow_Fault,         /* consecutive over-limits, or a starved re-check */
} flow_verdict_t;

/* ------------------------------------------------------------- state */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t engine_th;
static int engine_run = 0;

/* Controller report (HTTP thread writes, engine thread reads). */
static int rep_mode = 0;               /* 0 idle, 1 run, 2 cooldown */
static int rep_armed = 0;
static long rep_duty[3] = {-1, -1, -1}; /* air_assist, exhaust, intake */
static double rep_at = -1.0;           /* CLOCK_MONOTONIC; <0 = never */

/* Published snapshot for cool_status_json. */
static char pub_verdict[12] = "OK";
static char pub_reason[112];
static int pub_fire_ok, pub_hold;
static double pub_down = -273.15, pub_up = -273.15;
static const char *pub_phase = "idle";

/* Engine-thread internals (no locking needed). */
static cool_state_t cool_state = Cool_Idle;
static flow_verdict_t flow_verdict = Flow_Normal;
static uint32_t smoke_s = COOLDOWN_SMOKE_S;
static uint32_t cooldown_max_s = COOLDOWN_MAX_S;
static float temp_max_c = TEMP_MAX_C_DEFAULT;
static float temp_resume_c = TEMP_RESUME_C_DEFAULT;
static float flow_fault_rise = FLOW_FAULT_RISE_C;
static uint32_t flow_heater_pct = FLOW_HEATER_PCT;
static uint32_t flow_check_s = FLOW_CHECK_S;
static uint32_t flow_recheck_s = FLOW_RECHECK_S;
static uint32_t confirm_max_s = FLOW_CONFIRM_MAX_S;
static double flow_next_check;
static int flow_check_active = 0;
static int flow_check_pending = 0;
static double flow_pending_since;
static int flow_settle_warned = 0;
static int flow_base_set = 0;
static float down_hist[FLOW_SETTLE_WIN];
static uint32_t down_hist_n = 0;
static float flow_base_down;
static double flow_establish_at, flow_check_end;
static float flow_dt_sum;
static uint32_t flow_dt_n;
static double flow_suspect_since;
static uint32_t flow_episodes = 0;  /* cleared suspicions this job */
static double phase_until;          /* smoke end / thermal timeout */
static int over_temp_gate = 0;      /* hysteresis: >max sets, <=resume clears */
static int forced_cool = 0;         /* over-temp overrode the phase fans */
static int flood_on = 0;            /* effective run window */
static int silent_warned = 0;
static int diag_had = 0;            /* diagnostics held the hardware */
static long run_duty[3] = {-1, -1, -1};
static unsigned long verdict_seq = 0;

static double wall_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

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

static void wr_attr_long(const char *attr, long val)
{
    char v[24];
    snprintf(v, sizeof(v), "%ld", val);
    wr_attr(attr, v);
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

static int read_temp(const char *attr, float *c)
{
    long raw = rd_long(attr);
    if (raw < 0)
        return 0;
    *c = (float)coolant_degc(raw);
    return 1;
}

static void heater_set_pct(uint32_t pct)
{
    wr_attr_long("thermal/heater_pwm", (long)pct * 65535 / 100);
}

static void fans_idle(void)
{
    wr_attr_long("head/air_assist_pwm", AIR_ASSIST_IDLE);
    wr_attr_long("thermal/exhaust_pwm", EXHAUST_IDLE);
    wr_attr_long("thermal/intake_pwm", INTAKE_IDLE);
}

/* Run duties: the per-job report profile when given, factory values
 * otherwise. */
static void fans_run(void)
{
    wr_attr_long("head/air_assist_pwm",
                 run_duty[0] >= 0 ? run_duty[0] : AIR_ASSIST_RUN);
    wr_attr_long("thermal/exhaust_pwm",
                 run_duty[1] >= 0 ? run_duty[1] : EXHAUST_RUN);
    wr_attr_long("thermal/intake_pwm",
                 run_duty[2] >= 0 ? run_duty[2] : INTAKE_RUN);
}

static void fans_cool(void)
{
    wr_attr_long("head/air_assist_pwm", AIR_ASSIST_IDLE);
    wr_attr_long("thermal/exhaust_pwm", EXHAUST_COOL);
    wr_attr_long("thermal/intake_pwm", INTAKE_COOL);
}

/* Reapply the fan profile the current phase calls for (used when an
 * over-temp intervention forced cooling airflow and then stood down). */
static void fans_apply_phase(void)
{
    switch (cool_state) {
    case Cool_Run:
    case Cool_Smoke:
        fans_run();
        break;
    case Cool_Thermal:
        fans_cool();
        break;
    default:
        fans_idle();
        break;
    }
}

/* ----------------------------------------------------------- logging */

static void warn(const char *msg)
{
    pthread_mutex_lock(&mu);
    snprintf(pub_reason, sizeof(pub_reason), "%s", msg);
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "forgectrl: cool: %s\n", msg);
}

static void info(const char *msg)
{
    fprintf(stderr, "forgectrl: cool: %s\n", msg);
}

/* ---------------------------------------------------------- tunables */

/* Tunables resolve env > settings > compiled default. The GFCOOL_* env
 * vars are the bench-debug path and win for the process lifetime; the
 * cool_* keys in the shared machine settings are the user-facing store
 * (Machine tab) and are re-read at every run start, so GUI changes
 * apply from the next job with no restart. */
static struct {
    const char *env, *key;
    float def;
    float *f;                       /* exactly one of f/u is set */
    uint32_t *u;
    int env_set;
} tunables[] = {
    { "GFCOOL_FLOW_HEATER_PCT", "cool_flow_heater_pct",
      FLOW_HEATER_PCT,        NULL,             &flow_heater_pct, 0 },
    { "GFCOOL_FLOW_CHECK_S",    "cool_flow_check_s",
      FLOW_CHECK_S,           NULL,             &flow_check_s, 0 },
    { "GFCOOL_RECHECK_S",       "cool_recheck_s",
      FLOW_RECHECK_S,         NULL,             &flow_recheck_s, 0 },
    { "GFCOOL_COOLDOWN_S",      "cool_cooldown_s",
      COOLDOWN_SMOKE_S,       NULL,             &smoke_s, 0 },
    { "GFCOOL_COOLDOWN_MAX_S",  "cool_cooldown_max_s",
      COOLDOWN_MAX_S,         NULL,             &cooldown_max_s, 0 },
    { "GFCOOL_TEMP_MAX",        "cool_temp_max",
      TEMP_MAX_C_DEFAULT,     &temp_max_c,      NULL, 0 },
    { "GFCOOL_TEMP_RESUME",     "cool_temp_resume",
      TEMP_RESUME_C_DEFAULT,  &temp_resume_c,   NULL, 0 },
    { "GFCOOL_FLOW_RISE",       "cool_flow_rise",
      FLOW_FAULT_RISE_C,      &flow_fault_rise, NULL, 0 },
    { "GFCOOL_CONFIRM_MAX_S",   "cool_confirm_max_s",
      FLOW_CONFIRM_MAX_S,     NULL,             &confirm_max_s, 0 },
};

static void conf_reload(void)
{
    for (size_t i = 0; i < sizeof(tunables) / sizeof(tunables[0]); i++) {
        if (tunables[i].env_set)
            continue;
        char v[32];
        char *end;
        float f = tunables[i].def;
        if (settings_get(tunables[i].key, v, sizeof(v)) == 0) {
            float parsed = strtof(v, &end);
            if (end != v && *end == '\0')
                f = parsed;
        }
        if (tunables[i].f)
            *tunables[i].f = f;
        else
            *tunables[i].u = f < 0.0f ? 0 : (uint32_t)f;
    }
}

/* ------------------------------------------------------ verdict file */

/* Atomic publish: write-temp + rename, ~1 Hz and on every change (the
 * loop runs at 1 Hz, so every iteration publishes). Readers treat a
 * missing file or ts_mono older than ~2 s as fire_ok=false, hold=true. */
static void verdict_publish(int fire_ok, const char *verdict, int hold,
                            int have_down, float down,
                            int have_up, float up)
{
    char body[320];
    pthread_mutex_lock(&mu);
    snprintf(pub_verdict, sizeof(pub_verdict), "%s", verdict);
    pub_fire_ok = fire_ok;
    pub_hold = hold;
    pub_down = have_down ? down : -273.15;
    pub_up = have_up ? up : -273.15;
    int n = snprintf(body, sizeof(body),
        "{\"seq\":%lu,\"ts_mono\":%.3f,\"fire_ok\":%s,"
        "\"verdict\":\"%s\",\"hold\":%s,\"resume_ok\":%s,"
        "\"reason\":\"%s\",\"down_c\":%.2f,\"up_c\":%.2f}\n",
        ++verdict_seq, wall_s(), fire_ok ? "true" : "false",
        verdict, hold ? "true" : "false", hold ? "false" : "true",
        pub_reason, pub_down, pub_up);
    pthread_mutex_unlock(&mu);

    int fd = open(VERDICT_TMP, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return;
    int ok = write(fd, body, (size_t)n) == n;
    close(fd);
    if (ok)
        rename(VERDICT_TMP, VERDICT_FILE);
}

/* ------------------------------------------------------------ engine */

/* Begin a flow check: heater up, baseline captured on the next poll,
 * verdict at the end of the window, heater back off. */
static void flow_check_start(double now)
{
    flow_check_active = 1;
    flow_base_set = 0;
    flow_establish_at = now + FLOW_ESTABLISH_S;
    flow_check_end = now + (double)flow_check_s;
    flow_dt_sum = 0.0f;
    flow_dt_n = 0;
    heater_set_pct(flow_heater_pct);
}

/* Enter/leave the effective run window: reported mode "run" OR armed -
 * fire must never happen without the cut airflow and the flow
 * interrogation that only lives inside a run session. */
static void flood_apply(int on, double now)
{
    if (on) {
        conf_reload();              /* GUI changes apply per job */
        cool_state = Cool_Run;
        fans_run();
        if (flow_verdict == Flow_Suspect)
            flow_suspect_since = now;   /* budget is per run session */
        /* Request a check at run start; the loop starts it once the
         * loop is settled, then repeats it on the re-check cadence. */
        if (flow_check_s > 0 && !flow_check_active && !flow_check_pending) {
            flow_check_pending = 1;
            flow_pending_since = now;
            flow_settle_warned = 0;
        }
    } else if (cool_state == Cool_Run) {
        cool_state = Cool_Smoke;
        phase_until = now + (double)smoke_s;
        flow_check_pending = 0;
        if (flow_check_active) {    /* run ended before the verdict */
            flow_check_active = 0;
            heater_set_pct(0);
        }
    }
}

/* One engine tick at 1 Hz. Hardware writes happen only on transitions
 * so the engine coexists with a not-yet-migrated in-controller writer
 * during bring-up. */
static void engine_tick(void)
{
    double now = wall_s();

    /* Diagnostics own the hardware while they run; hold everything and
     * publish fire-blocked until they hand back. */
    if (diag_running()) {
        if (!diag_had) {
            diag_had = 1;
            flood_apply(0, now);
            warn("diagnostics own the thermal hardware");
        }
        pthread_mutex_lock(&mu);
        pub_phase = "diag";
        pthread_mutex_unlock(&mu);
        verdict_publish(0, "OK", 1, 0, 0, 0, 0);
        return;
    }
    if (diag_had) {
        diag_had = 0;
        pthread_mutex_lock(&mu);
        pub_reason[0] = '\0';
        pthread_mutex_unlock(&mu);
        /* diag stood the loop down to idle; reassert our phase. */
        wr_attr("thermal/water_pump_on", "1");
        fans_apply_phase();
    }

    /* Snapshot the report. */
    pthread_mutex_lock(&mu);
    int mode = rep_mode, armed = rep_armed;
    double at = rep_at;
    long duty[3] = {rep_duty[0], rep_duty[1], rep_duty[2]};
    pthread_mutex_unlock(&mu);

    int fresh = at >= 0 && now - at <= REPORT_TIMEOUT_S;
    if (!fresh) {
        mode = 0;
        armed = 0;
        if (at >= 0 && !silent_warned && (flood_on || cool_state == Cool_Run)) {
            silent_warned = 1;
            warn("controller went silent - standing down");
        }
    } else
        silent_warned = 0;

    int duty_changed = memcmp(run_duty, duty, sizeof(run_duty)) != 0;
    memcpy(run_duty, duty, sizeof(run_duty));
    if (duty_changed && flood_on && cool_state == Cool_Run && !forced_cool)
        fans_run();     /* per-job profile changed mid-run */

    int flood = fresh && (mode == 1 || armed);
    if (flood != flood_on) {
        flood_on = flood;
        flood_apply(flood, now);
        if (flood) {
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';   /* new session, stale reason gone */
            pthread_mutex_unlock(&mu);
        }
    }

    float down = 0, up = 0;
    int have_down = read_temp("pic/water_temp_1", &down);
    int have_up = read_temp("pic/water_temp_2", &up);

    if (have_up) {
        /* Fire gate with hysteresis: gated over the run ceiling, back
         * in service under the resume gate. The hold request rides the
         * same gate: the controller holds while it stands, resumes
         * (its call) once resume_ok returns. */
        int was = over_temp_gate;
        over_temp_gate = up > (over_temp_gate ? temp_resume_c : temp_max_c);
        if (over_temp_gate && !was) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "coolant %.1f C over %.0f C limit - hold until %.0f C",
                     up, temp_max_c, temp_resume_c);
            warn(msg);
            if (cool_state != Cool_Run) {
                fans_cool();
                forced_cool = 1;
            }
        } else if (!over_temp_gate && was) {
            info("coolant temperature recovered");
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
            if (forced_cool) {
                forced_cool = 0;
                fans_apply_phase();
            }
        }
    }

    /* One-shot flow check: how far does the downstream sensor climb
     * while the heater runs? Flow carries the heat away (plateau);
     * no flow lets it pile up. The delta is averaged too, but only for
     * the report - it is the weaker discriminator (see file header). */
    if (flow_check_active && have_down && have_up) {
        if (!flow_base_set) {
            flow_base_down = down;
            flow_base_set = 1;
        }
        if (now >= flow_establish_at) {
            flow_dt_sum += down - up;
            flow_dt_n++;
        }
        if (now >= flow_check_end) {
            float rise = down - flow_base_down;
            float dt = flow_dt_n ? flow_dt_sum / (float)flow_dt_n : 0.0f;
            char msg[112];

            flow_check_active = 0;
            heater_set_pct(0);
            /* Schedule the next interrogation if the run is still on. */
            flow_next_check = now + (double)flow_recheck_s;

            if (rise > flow_fault_rise) {
                if (flow_verdict == Flow_Normal) {
                    /* First over-limit: open a suspicion and re-check
                     * as soon as the loop settles instead of waiting
                     * out the cadence. */
                    flow_verdict = Flow_Suspect;
                    flow_suspect_since = now;
                    flow_check_pending = 1;
                    flow_pending_since = now;
                    flow_settle_warned = 0;
                    snprintf(msg, sizeof(msg),
                             "COOLANT FLOW SUSPECT: heater rise %.1f C (limit %.1f, dT %.1f) - re-checking",
                             rise, flow_fault_rise, dt);
                } else {
                    flow_verdict = Flow_Fault;
                    snprintf(msg, sizeof(msg),
                             "COOLANT FLOW FAULT: heater rise %.1f C (limit %.1f, dT %.1f) - check the pump",
                             rise, flow_fault_rise, dt);
                }
                warn(msg);
            } else if (flow_verdict != Flow_Normal) {
                snprintf(msg, sizeof(msg),
                         flow_verdict == Flow_Suspect
                           ? "coolant flow suspicion cleared (heater rise %.1f C, dT %.1f C)"
                           : "coolant flow recovered (heater rise %.1f C, dT %.1f C)",
                         rise, dt);
                flow_verdict = Flow_Normal;
                info(msg);
                pthread_mutex_lock(&mu);
                pub_reason[0] = '\0';
                pthread_mutex_unlock(&mu);
                if (++flow_episodes == FLOW_TREND_N) {
                    snprintf(msg, sizeof(msg),
                             "%u coolant flow suspicions this job - check the pump and coolant level",
                             (unsigned)flow_episodes);
                    warn(msg);
                }
            } else {
                snprintf(msg, sizeof(msg),
                         "coolant flow verified (heater rise %.1f C, dT %.1f C)",
                         rise, dt);
                info(msg);
            }
        }
    } else if (cool_state == Cool_Run && flow_check_s > 0 && flow_recheck_s > 0
               && !flow_check_pending && now >= flow_next_check) {
        flow_check_pending = 1;     /* periodic re-interrogation mid-run */
        flow_pending_since = now;
        flow_settle_warned = 0;
    }

    /* A suspicion may not sit unresolved while a run is on: no verdict
     * within the budget (settle gate starved - itself consistent with
     * stagnation, which decays by conduction only) fails safe. An
     * in-flight check is left to deliver its real verdict instead. */
    if (flow_verdict == Flow_Suspect && !flow_check_active
        && cool_state == Cool_Run
        && now - flow_suspect_since > (double)confirm_max_s) {
        char msg[96];
        flow_verdict = Flow_Fault;
        snprintf(msg, sizeof(msg),
                 "COOLANT FLOW FAULT: no clean re-check within %u s - check the pump",
                 (unsigned)confirm_max_s);
        warn(msg);
    }

    /* Track downstream history for the stationarity test below. It is
     * only meaningful while the heater is off, so a running check
     * invalidates it. */
    if (have_down) {
        if (flow_check_active)
            down_hist_n = 0;
        else {
            down_hist[down_hist_n % FLOW_SETTLE_WIN] = down;
            down_hist_n++;
        }
    }

    /* Gate: a pending check only starts from a settled loop - sensors
     * in agreement AND the downstream reading stationary. */
    if (flow_check_pending && !flow_check_active && have_down && have_up) {
        int stationary = down_hist_n >= FLOW_SETTLE_WIN;
        if (stationary) {
            uint32_t base = down_hist_n - FLOW_SETTLE_WIN;
            float old_sum = 0.0f, new_sum = 0.0f;
            for (uint32_t i = 0; i < 7; i++)
                old_sum += down_hist[(base + i) % FLOW_SETTLE_WIN];
            for (uint32_t i = 7; i < FLOW_SETTLE_WIN; i++)
                new_sum += down_hist[(base + i) % FLOW_SETTLE_WIN];
            stationary =
                fabsf(new_sum / 8.0f - old_sum / 7.0f) <= FLOW_SETTLE_DRIFT_C;
        }
        if (stationary && fabsf(down - up) <= FLOW_SETTLE_DT_C) {
            flow_check_pending = 0;
            flow_check_start(now);
        } else if (!flow_settle_warned
                   && now - flow_pending_since > FLOW_SETTLE_WARN_S) {
            flow_settle_warned = 1;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "flow check deferred: loop not settled (dT %.1f C, %s)",
                     down - up, stationary ? "drifting" : "sensors disagree");
            warn(msg);
        }
    }

    /* Cooldown phases. */
    if (cool_state == Cool_Smoke && now >= phase_until) {
        if (have_up && up > temp_resume_c) {
            cool_state = Cool_Thermal;
            phase_until = now + (double)cooldown_max_s;
            fans_cool();
        } else {
            cool_state = Cool_Idle;
            fans_idle();
            flow_episodes = 0;
        }
    } else if (cool_state == Cool_Thermal) {
        int recovered = have_up && up <= temp_resume_c;
        if (recovered || now >= phase_until) {
            if (!recovered)
                warn("thermal cooldown timed out above the resume gate");
            cool_state = Cool_Idle;
            fans_idle();
            flow_episodes = 0;
        }
    }

    /* Publish. Enforcement is the controller's: hold asks for a feed
     * hold, resume_ok (= !hold) signals recovery, fire_ok gates the
     * laser. fire_ok additionally requires a live report: an armed
     * window the engine cannot see must not fire. */
    const char *verdict = over_temp_gate ? "OVERTEMP"
                        : flow_verdict == Flow_Fault ? "FAULT"
                        : flow_verdict == Flow_Suspect ? "SUSPECT" : "OK";
    int hold = over_temp_gate || (armed && flow_verdict != Flow_Normal);
    int fire_ok = fresh && !over_temp_gate && flow_verdict != Flow_Fault;

    pthread_mutex_lock(&mu);
    pub_phase = cool_state == Cool_Run ? "run"
              : cool_state == Cool_Smoke ? "smoke"
              : cool_state == Cool_Thermal ? "thermal" : "idle";
    pthread_mutex_unlock(&mu);

    verdict_publish(fire_ok, verdict, hold, have_down, down, have_up, up);
}

static void *engine_main(void *arg)
{
    (void)arg;
    while (1) {
        pthread_mutex_lock(&mu);
        int run = engine_run;
        pthread_mutex_unlock(&mu);
        if (!run)
            break;
        engine_tick();
        sleep(1);
    }
    return NULL;
}

/* --------------------------------------------------------------- api */

void cool_init(void)
{
    const char *opt;
    for (size_t i = 0; i < sizeof(tunables) / sizeof(tunables[0]); i++) {
        if ((opt = getenv(tunables[i].env))) {
            tunables[i].env_set = 1;
            if (tunables[i].f)
                *tunables[i].f = strtof(opt, NULL);
            else
                *tunables[i].u = (uint32_t)strtoul(opt, NULL, 10);
        }
    }
    conf_reload();

    mkdir(VERDICT_DIR, 0755);

    /* Idle posture. Purge air belongs to the armed/run story the same
     * way air assist does, but the factory holds it on continuously;
     * keep that. */
    wr_attr("thermal/water_pump_on", "1");
    wr_attr("thermal/tec_on", "0");
    wr_attr("head/purge_air", "1");
    heater_set_pct(0);
    fans_idle();

    engine_run = 1;
    if (pthread_create(&engine_th, NULL, engine_main, NULL) != 0) {
        engine_run = 0;
        fprintf(stderr, "forgectrl: cool: engine thread failed to start\n");
        return;
    }
    fprintf(stderr, "forgectrl: cool: engine started\n");
}

void cool_shutdown(void)
{
    pthread_mutex_lock(&mu);
    int was = engine_run;
    engine_run = 0;
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(engine_th, NULL);
    heater_set_pct(0);
    fans_idle();
    unlink(VERDICT_FILE);   /* missing file = fire blocked at readers */
}

int cool_state_report(const char *mode, int armed,
                      long air_assist, long exhaust, long intake)
{
    int m;
    if (!strcmp(mode, "idle"))
        m = 0;
    else if (!strcmp(mode, "run"))
        m = 1;
    else if (!strcmp(mode, "cooldown"))
        m = 2;
    else
        return -1;

    pthread_mutex_lock(&mu);
    rep_mode = m;
    rep_armed = !!armed;
    rep_duty[0] = air_assist >= 0 && air_assist <= 1023 ? air_assist : -1;
    rep_duty[1] = exhaust >= 0 && exhaust <= 65535 ? exhaust : -1;
    rep_duty[2] = intake >= 0 && intake <= 65535 ? intake : -1;
    rep_at = wall_s();
    pthread_mutex_unlock(&mu);
    return 0;
}

double cool_report_age(void)
{
    pthread_mutex_lock(&mu);
    double age = rep_at < 0 ? -1.0 : wall_s() - rep_at;
    pthread_mutex_unlock(&mu);
    return age;
}

int cool_status_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    double age = rep_at < 0 ? -1 : wall_s() - rep_at;
    snprintf(buf, len,
        "{\"phase\":\"%s\",\"verdict\":\"%s\",\"fire_ok\":%s,"
        "\"hold\":%s,\"reason\":\"%s\",\"down_c\":%.2f,\"up_c\":%.2f,"
        "\"report_age_s\":%.1f,\"armed\":%s}",
        pub_phase, pub_verdict, pub_fire_ok ? "true" : "false",
        pub_hold ? "true" : "false", pub_reason, pub_down, pub_up,
        age, rep_armed ? "true" : "false");
    pthread_mutex_unlock(&mu);
    return 0;
}

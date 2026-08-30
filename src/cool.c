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
 * - GATE SETTINGS (gates.c): the ceiling, its resume gate, the flow
 *   window and the flow rise are plain settings with a wide legal
 *   range, a recommended band and an off end; a ceiling at its top or
 *   a flow window of zero turns that gate off by value. An off gate is
 *   skipped, still measured, logged at every run start, and published
 *   as gates_off.
 * - AIRFLOW GATES (airflow.c): while the run profile is applied, each
 *   fan is held to a floor (the exhaust, intakes and air assist by
 *   tachometer, the purge fan by current) after a spin-up grace;
 *   three consecutive ticks under the floor trip a fault for the
 *   rest of the run session (verdict AIRFLOW: hold, fire blocked, no
 *   resume), the fans held at run duty. A header's tach window can
 *   raise a floor for a job, never lower it.
 * - Physical-evidence witnesses (1 Hz, alongside the loop): the
 *   kernel's sampled LASER_ON count is the ground truth of emission
 *   (the gated output of the hardware AND-gate, not a commanded
 *   state). Emission sensed with no armed window in the recent past
 *   stops motion and locks the latch; laser power-good degradation
 *   during an armed window is warned. The four lid IR channels are
 *   polled every tick: their run-start baseline and session peaks are
 *   logged every job (the commissioning dataset), and when
 *   cool_fire_ir_delta is nonzero a sustained rise above baseline on
 *   any channel is a FIRE signal - motion stopped, latch locked,
 *   verdict FIRE with hold until the next run session. The delta ships
 *   0 (watch-only) until the sensors are characterized on the bench.
 * - Controller silence: if the active controller stops reporting past
 *   REPORT_TIMEOUT_S, fire_ok goes false immediately and the engine
 *   stands down through the normal cooldown path (smoke clear is the
 *   right physical behavior for a job that died mid-cut). Silence with
 *   the armed window open - or with the kernel still playing the ring
 *   (cloud mode preloads whole jobs, so the ring can run for minutes
 *   with no live feeder) - additionally stops motion and locks the
 *   laser latch right here: the supervisor safes controller *deaths*,
 *   this covers controller *hangs*. And while cnc/state still reads
 *   running, exhaust/intake never drop below cooldown duty.
 * - Diagnostics (diag.c) own the hardware while they run: the engine
 *   suspends its writes and publishes fire-blocked until they finish.
 */
#define _GNU_SOURCE
#include "airflow.h"
#include "cool.h"
#include "coolfmt.h"
#include "diag.h"
#include "fflog.h"
#include "gates.h"
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

#ifndef GF_SYSFS                /* the host test points these at a fake tree */
#define GF_SYSFS     "/sys/glowforge/"
#endif
#ifndef VERDICT_DIR
#define VERDICT_DIR  "/run/forgefirm"
#endif
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
#define TEMP_CRITICAL_C_DEFAULT 38.0f
/* Airflow floors (gates.c carries the rationale and the ranges). */
#define TACH_EXHAUST_MIN_RPM     6400.0f
#define TACH_INTAKE_MIN_RPM      2290.0f
#define TACH_AIR_ASSIST_MIN_RPM  6000.0f
#define PURGE_MIN_CURRENT        300.0f
#define FAN_GRACE_S              15.0f

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

/* The rise is read from means, never from single samples. The coolant
 * ADC carries a common-mode offset of about 1 C while the run airflow
 * profile is on: it steps in at the session open, out when the fans go
 * idle, and toggles between two levels in between, on both sensors
 * together. One sample at the start of the check and one at its end
 * can therefore differ by a degree with no heat behind it. The
 * baseline is the mean of the settled window the gate has just verified
 * (FLOW_SETTLE_WIN samples); the end reading is the mean of the check's
 * last FLOW_END_WIN samples. */
#define FLOW_END_WIN        5       /* 1 Hz samples */

/* The tube warms the same loop the heater does, and with a prompt press
 * the tube is lit for most of the arm-time window. Measured on the
 * bench (four CW bursts of 20 to 60 s, the offset steps masked): the
 * downstream rise at burst end is linear in the integral of
 * pic/hv_current, 3.06e-5 C per raw-second (r2 0.98), 0.030 C per lit
 * second at full duty, so a window lit from its start carries about
 * 1.5 C of tube heat against a 1.6 C margin. Under the density model
 * the heat per raw-second is 0.77 of that: every pulse pays a strike
 * delay the current sees and the tube does not. The heat reaches the
 * sensor 10 to 20 s after emission, so emission in the window's last
 * FLOW_LAG_S seconds is not counted (under-correcting, the safe side)
 * and emission in the FLOW_LAG_S seconds before the window is. The
 * share is subtracted from the measured rise before the limit, and it
 * is bounded so that no setting can subtract the check away: a stopped
 * pump reads 16 C and more, the bound leaves 13 C of that. */
#define FLOW_LAG_S          15      /* 1 Hz samples of hv history */
#define LASER_HEAT_CW       3.06e-5f    /* C per raw-second, full duty */
#define LASER_HEAT_DENSITY  2.36e-5f    /* 0.77 of it under density */
#define LASER_HEAT_MAX_C    3.0f    /* the most a check may subtract */

/* The air-assist fan's return current shares a ground with the coolant
 * thermistors' reference, so both sensors read low by a voltage offset,
 * constant in ADC counts, while the fan runs: measured on the bench
 * about 20 counts (1.2 C near 22 C) at the run duty 1023, nothing below
 * the fan's start (256), and in between in proportion to the fan's
 * current. More counts read colder in this conversion, so the lift reads
 * as a drop. The engine commands that fan, so it knows the duty at every
 * tick and takes the lift off both raw readings before conversion; the
 * same correction goes into /status through
 * cool_coolant_offset_counts(). The value is the machine's own
 * (cool_aa_offset_counts, measured by the aa-offset-calibrate
 * diagnostic); zero, the default, is the factory behavior, which never
 * corrected it. The flow check is indifferent (its baseline and end are
 * read under the same duty); the over-temperature gates are what the
 * correction is for. Bounded so no setting can move the ceiling far. */
#define AA_OFFSET_START      256    /* the fan does not turn below this duty */
#define AA_OFFSET_FULL       1023   /* the duty the setting is measured at */
#define AA_OFFSET_MAX_COUNTS 60.0f

/* Lid IR fire watch: ADC counts of rise above the run-start baseline
 * that read as a fire signal, sustained for FIRE_IR_TICKS consecutive
 * ticks. 0 = watch-only (log the dataset, never trip) - the shipped
 * default until the sensors are characterized on the bench and
 * cool_fire_ir_delta is set. GFCOOL_FIRE_IR_DELTA overrides. */
#define FIRE_IR_DELTA      0
#define FIRE_IR_TICKS      2

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
static cool_limits_t rep_lim = {-1, -1, -1, -1, -1}; /* <= 0 = absent */
static double rep_at = -1.0;           /* CLOCK_MONOTONIC; <0 = never */

/* Published snapshot for cool_status_json. */
static char pub_verdict[COOL_VERDICT_MAX] = "OK";
static char pub_reason[COOL_REASON_MAX];
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
static float temp_critical_c = TEMP_CRITICAL_C_DEFAULT;
static float tach_exhaust_min_rpm = TACH_EXHAUST_MIN_RPM;
static float tach_intake_min_rpm = TACH_INTAKE_MIN_RPM;
static float tach_air_assist_min_rpm = TACH_AIR_ASSIST_MIN_RPM;
static float purge_min_current = PURGE_MIN_CURRENT;
static float fan_grace_s = FAN_GRACE_S;
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
static float flow_end_hist[FLOW_END_WIN];
static uint32_t flow_end_n;
static long hv_hist[FLOW_LAG_S];    /* pic/hv_current, the last FLOW_LAG_S ticks */
static uint32_t hv_hist_n;
static long hv_last = -1;           /* this tick's reading, -1 = none */
static double flow_hv_dose;         /* raw-seconds counted for this check */
static float flow_laser_c;          /* the tube's share of the rise, this check */
static float laser_heat_cw = LASER_HEAT_CW;
static float laser_heat_density = LASER_HEAT_DENSITY;
static int laser_model_density = 1; /* the model in force, read per run */
static int rep_model = -1;          /* the controller's reported model, -1 unknown */
static float aa_offset_counts = 0.0f;   /* cool_aa_offset_counts, at duty 1023 */
static long aa_cmd = 204;               /* the air-assist duty last commanded */
static double phase_until;          /* smoke end / thermal timeout */
static int over_temp_gate = 0;      /* hysteresis: >max sets, <=resume clears */
static int critical_alarm = 0;      /* latched coolant fault, this run session */
/* The board temperatures, watched over the run session and named once
 * at its end; no gate behind them until the data says where one goes. */
static double job_chassis_lo, job_chassis_hi;
static double job_soc_lo, job_soc_hi;
static long job_supply_lo, job_supply_hi;
static int job_temps_seen = 0;
static int job_throttled = 0;       /* ticks the CPU was throttled this job */
static long throttle_state = 0;     /* the kernel's CPU cooling state last seen */
static int critical_would_warned = 0;   /* off gate, would have tripped: once */
static int forced_cool = 0;         /* over-temp overrode the phase fans */
static int flood_on = 0;            /* effective run window */
static int silent_warned = 0;
static int silent_safed = 0;        /* hang dead-man fired this episode */

/* Physical-evidence witnesses. */
static uint32_t fire_ir_delta = FIRE_IR_DELTA;  /* 0 = watch-only */
static double last_armed_at = -1.0; /* last fresh armed report seen */
static int emission_warned = 0;
static int pgood_warned = 0;        /* once per run session */
static long last_faults = 0;
static long ir_base[4] = {-1, -1, -1, -1};  /* run-session baseline */
static long ir_peak[4];
static int ir_over_ticks = 0;
static int fire_alarm = 0;          /* latched lid-IR fire signal */
static long hv_lo = -1, hv_hi = -1; /* session hv_current range */
static const char *pub_fire_watch = "watch";
static int diag_had = 0;            /* diagnostics held the hardware */
static long run_duty[3] = {-1, -1, -1};
static long cmd_duty[3] = {-1, -1, -1};   /* what fans_run last wrote */
static int eff_armed = 0;                 /* the armed window, as reported */
static unsigned long verdict_seq = 0;

/* Gate settings at their off end (gates.c): the coolant ceiling at its
 * top and the flow window at zero. An off gate is skipped, not
 * evaluated, and still measured: the first reading in a run session
 * that would have tripped the shipped default is logged once. */
static int gate_coolant_off = 0;
static int gate_critical_off = 0;
static int gate_flow_off = 0;
static int off_would_warned = 0;
static char pub_gates_off[COOL_GATES_OFF_JSON_MAX] = "[]";

/* Effective limits: the local tunable, tightened by the job's header
 * where the report carries a stricter value (gate_effective). The
 * coolant ceiling is the one with a gate behind it today; the floors
 * are carried, logged and published so the gates that follow find
 * them in place. eff_resume follows a tightened ceiling down by the
 * configured gap. Logged whenever the effective set changes. */
static float eff_temp_max_c = TEMP_MAX_C_DEFAULT;
static float eff_temp_resume_c = TEMP_RESUME_C_DEFAULT;
static int eff_from_header = 0;          /* the ceiling is the header's */
static cool_limits_t eff_lim = {-1, -1, -1, -1, -1};
static cool_limits_t last_logged_lim = {-2, -2, -2, -2, -2};
static float last_logged_max = -1.0f;
static double looser_warned_c = -1.0;    /* header ceiling already named */
static char pub_limits[COOL_LIMITS_JSON_MAX] = "{}";

/* The airflow gates (airflow.c): one per fan, evaluated while the run
 * profile is applied, after a grace window from the moment it was
 * written. A trip is a fault for the rest of the run session. */
enum { Fan_Exhaust, Fan_Intake1, Fan_Intake2, Fan_AirAssist, Fan_Purge, Fan_N };
static const char *fan_name[Fan_N] = {"exhaust", "intake_1", "intake_2", "air_assist", "purge"};
static airflow_fan_t fan_gate[Fan_N];
static double fan_reading[Fan_N];
static double fan_floor[Fan_N];
static double fan_grace_until = 0.0;
static int airflow_alarm = 0;        /* latched fan fault, this run session */
static int fan_would_warned = 0;     /* off gate, would have tripped: once */
static char pub_fan_gates[COOL_FAN_GATES_JSON_MAX] = "{}";

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
    /* The air-assist ground shift lifts the raw counts (more counts read
     * colder): take them off before the conversion. */
    long raw_c = raw - cool_coolant_offset_counts();
    *c = (float)coolant_degc(raw_c > 0 ? raw_c : raw);
    return 1;
}

/* cnc/state == "running" is the one state in which the machine can be
 * depositing energy (the ring is being clocked). A read failure counts
 * as not-running here: this feeds extra protective actions, and the
 * fire gate itself fails closed elsewhere. */
static int cnc_is_running(void)
{
    char path[128], buf[16];
    snprintf(path, sizeof(path), GF_SYSFS "cnc/state");
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return strncmp(buf, "running", 7) == 0;
}

static void heater_set_pct(uint32_t pct)
{
    wr_attr_long("thermal/heater_pwm", (long)pct * 65535 / 100);
}

/* Every air-assist write goes through here: the coolant readings'
 * correction follows the duty the engine last commanded. */
static void aa_write(long duty)
{
    aa_cmd = duty;
    wr_attr_long("head/air_assist_pwm", duty);
}

long cool_coolant_offset_counts(void)
{
    float k = aa_offset_counts;
    if (k <= 0.0f || aa_cmd <= AA_OFFSET_START)
        return 0;
    if (k > AA_OFFSET_MAX_COUNTS)
        k = AA_OFFSET_MAX_COUNTS;
    float frac = (float)(aa_cmd - AA_OFFSET_START) / (float)(AA_OFFSET_FULL - AA_OFFSET_START);
    if (frac > 1.0f)
        frac = 1.0f;
    return (long)(k * frac + 0.5f);
}

static void fans_idle(void)
{
    aa_write(AIR_ASSIST_IDLE);
    wr_attr_long("thermal/exhaust_pwm", EXHAUST_IDLE);
    wr_attr_long("thermal/intake_pwm", INTAKE_IDLE);
}

/* Run duties: the per-job report profile when given, factory values
 * otherwise. */
/* The run profile: the configured duties, or the job's own where it
 * gave one, which may only raise a fan while the laser is armed
 * (airflow_run_duty). */
static void fans_run(void)
{
    cmd_duty[0] = airflow_run_duty(AIR_ASSIST_RUN, run_duty[0], eff_armed);
    cmd_duty[1] = airflow_run_duty(EXHAUST_RUN, run_duty[1], eff_armed);
    cmd_duty[2] = airflow_run_duty(INTAKE_RUN, run_duty[2], eff_armed);
    aa_write(cmd_duty[0]);
    wr_attr_long("thermal/exhaust_pwm", cmd_duty[1]);
    wr_attr_long("thermal/intake_pwm", cmd_duty[2]);
}

static void fans_cool(void)
{
    aa_write(AIR_ASSIST_IDLE);
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
    fflog(LOG_WARNING, "cool: %s", msg);
}

static float last_up_c = 0;         /* the upstream reading the hold was taken on */

/* The ceiling's hold, named: logged on the rising edge, and published
 * again (no new log line, it is the same hold) when a critical fault
 * that had overwritten the reason clears. */
static void overtemp_warn(int log)
{
    char msg[96];
    snprintf(msg, sizeof(msg),
             "coolant %.1f C over %.0f C limit%s - hold until %.0f C",
             last_up_c, eff_temp_max_c, eff_from_header ? " (the job's)" : "",
             eff_temp_resume_c);
    if (log) {
        warn(msg);
        return;
    }
    pthread_mutex_lock(&mu);
    snprintf(pub_reason, sizeof(pub_reason), "%s", msg);
    pthread_mutex_unlock(&mu);
}

static void info(const char *msg)
{
    fflog(LOG_INFO, "cool: %s", msg);
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
    { "GFCOOL_TEMP_CRITICAL",   "cool_temp_critical_c",
      TEMP_CRITICAL_C_DEFAULT, &temp_critical_c, NULL, 0 },
    { "GFCOOL_FLOW_RISE",       "cool_flow_rise",
      FLOW_FAULT_RISE_C,      &flow_fault_rise, NULL, 0 },
    { "GFCOOL_CONFIRM_MAX_S",   "cool_confirm_max_s",
      FLOW_CONFIRM_MAX_S,     NULL,             &confirm_max_s, 0 },
    { "GFCOOL_LASER_HEAT_CW",   "cool_laser_heat_cw",
      LASER_HEAT_CW,          &laser_heat_cw,   NULL, 0 },
    { "GFCOOL_LASER_HEAT_DENSITY", "cool_laser_heat_density",
      LASER_HEAT_DENSITY,     &laser_heat_density, NULL, 0 },
    { "GFCOOL_AA_OFFSET_COUNTS", "cool_aa_offset_counts",
      0.0f,                   &aa_offset_counts, NULL, 0 },
    { "GFCOOL_FIRE_IR_DELTA",   "cool_fire_ir_delta",
      FIRE_IR_DELTA,          NULL,             &fire_ir_delta, 0 },
    { "GFCOOL_TACH_EXHAUST_MIN", "cool_tach_exhaust_min_rpm",
      TACH_EXHAUST_MIN_RPM,   &tach_exhaust_min_rpm, NULL, 0 },
    { "GFCOOL_TACH_INTAKE_MIN",  "cool_tach_intake_min_rpm",
      TACH_INTAKE_MIN_RPM,    &tach_intake_min_rpm, NULL, 0 },
    { "GFCOOL_TACH_AIR_MIN",     "cool_tach_air_assist_min_rpm",
      TACH_AIR_ASSIST_MIN_RPM, &tach_air_assist_min_rpm, NULL, 0 },
    { "GFCOOL_PURGE_MIN",        "cool_purge_min_current",
      PURGE_MIN_CURRENT,      &purge_min_current, NULL, 0 },
    { "GFCOOL_FAN_GRACE_S",      "cool_fan_grace_s",
      FAN_GRACE_S,            &fan_grace_s,     NULL, 0 },
};

/* The engine's resolved value for a gate setting (env > settings >
 * default), for the gates table. */
static double engine_gate_value(const gate_setting_t *g, void *ctx)
{
    (void)ctx;
    if (!strcmp(g->key, "cool_temp_max"))
        return temp_max_c;
    if (!strcmp(g->key, "cool_temp_resume"))
        return temp_resume_c;
    if (!strcmp(g->key, "cool_temp_critical_c"))
        return temp_critical_c;
    if (!strcmp(g->key, "cool_flow_check_s"))
        return flow_check_s;
    if (!strcmp(g->key, "cool_flow_rise"))
        return flow_fault_rise;
    if (!strcmp(g->key, "cool_tach_exhaust_min_rpm"))
        return tach_exhaust_min_rpm;
    if (!strcmp(g->key, "cool_tach_intake_min_rpm"))
        return tach_intake_min_rpm;
    if (!strcmp(g->key, "cool_tach_air_assist_min_rpm"))
        return tach_air_assist_min_rpm;
    if (!strcmp(g->key, "cool_purge_min_current"))
        return purge_min_current;
    if (!strcmp(g->key, "cool_fan_grace_s"))
        return fan_grace_s;
    return g->def;
}

/* Classify every gate setting from the resolved tunables, publish the
 * gates that are off, and say so in the log: one line per setting,
 * at warning level when it is off or outside its band, so a machine
 * running without a gate says so at every run start. */
static void gates_apply(void)
{
    size_t n;
    const gate_setting_t *t = gate_settings(&n);
    for (size_t i = 0; i < n; i++) {
        double v = engine_gate_value(&t[i], NULL);
        gate_state_t st = gate_state(&t[i], v);
        if (t[i].gate && !strcmp(t[i].gate, "coolant_max"))
            gate_coolant_off = st == Gate_Off;
        if (t[i].gate && !strcmp(t[i].gate, "coolant_critical"))
            gate_critical_off = st == Gate_Off;
        if (t[i].gate && !strcmp(t[i].gate, "flow"))
            gate_flow_off = st == Gate_Off;
        if (st == Gate_Off)
            fflog(LOG_WARNING, "cool: gate %s OFF: %s = %g (the %s end of "
                  "%g to %g; recommended %g to %g, default %g)",
                  t[i].gate, t[i].key, v,
                  t[i].off_end < 0 ? "low" : "high", t[i].lo, t[i].hi,
                  t[i].band_lo, t[i].band_hi, t[i].def);
        else if (st == Gate_Warn)
            fflog(LOG_WARNING, "cool: %s = %g is outside the recommended "
                  "%g to %g (default %g)", t[i].key, v,
                  t[i].band_lo, t[i].band_hi, t[i].def);
        else
            fflog(LOG_INFO, "cool: %s = %g", t[i].key, v);
    }
    char off[sizeof(pub_gates_off)];
    if (gates_off_json(off, sizeof(off), engine_gate_value, NULL) < 0)
        snprintf(off, sizeof(off), "[]");
    pthread_mutex_lock(&mu);
    snprintf(pub_gates_off, sizeof(pub_gates_off), "%s", off);
    pthread_mutex_unlock(&mu);
}

/* Resolve the effective limits from the local tunables and the
 * latest fresh report's header limits (absent when the report carried
 * none, or is stale). The coolant ceiling is the one consumer; a
 * header ceiling applies only if the local gate is on (an operator
 * who turned the gate off at its far end is not overruled by a job)
 * and stricter than the local value, and the resume gate follows it
 * down by the configured gap. The floors are resolved the same way
 * and published for the gates that follow. Logged on every change of
 * the effective set, and a looser header value is named once per run
 * session. */
static void limits_apply(const cool_limits_t *hdr, int fresh)
{
    cool_limits_t none = {-1, -1, -1, -1, -1};
    const cool_limits_t *h = fresh && hdr ? hdr : &none;
    int from = 0;
    float max_c = temp_max_c, resume_c = temp_resume_c;
    if (!gate_coolant_off) {
        max_c = (float)gate_effective(temp_max_c, h->coolant_max_c, 0, &from);
        if (from)
            resume_c = max_c - (temp_max_c - temp_resume_c);
    }
    cool_limits_t eff;
    eff.coolant_max_c = max_c;
    eff.coolant_min_c = gate_effective(0.0, h->coolant_min_c, 1, NULL);
    /* A floor the operator set to zero is off and stays off; otherwise
     * a header floor can only raise it. */
    eff.exhaust_min_rpm = tach_exhaust_min_rpm <= 0.0f ? 0.0
        : gate_effective(tach_exhaust_min_rpm, h->exhaust_min_rpm, 1, NULL);
    eff.intake_min_rpm = tach_intake_min_rpm <= 0.0f ? 0.0
        : gate_effective(tach_intake_min_rpm, h->intake_min_rpm, 1, NULL);
    eff.air_assist_min_rpm = tach_air_assist_min_rpm <= 0.0f ? 0.0
        : gate_effective(tach_air_assist_min_rpm, h->air_assist_min_rpm, 1, NULL);

    int changed = max_c != last_logged_max ||
                  memcmp(&eff, &last_logged_lim, sizeof(eff)) != 0 ||
                  eff_from_header != from;
    eff_temp_max_c = max_c;
    eff_temp_resume_c = resume_c;
    eff_from_header = from;
    eff_lim = eff;
    if (changed) {
        last_logged_max = max_c;
        last_logged_lim = eff;
        fflog(LOG_INFO, "cool: effective limits: coolant ceiling %.1f C "
              "(local %.1f, header %s%.1f) resume %.1f C; floors coolant "
              "%.1f C (no gate yet), exhaust %.0f rpm, intake %.0f rpm, air "
              "assist %.0f rpm",
              max_c, temp_max_c, h->coolant_max_c > 0 ? "" : "none ",
              h->coolant_max_c > 0 ? h->coolant_max_c : 0.0, resume_c,
              eff.coolant_min_c, eff.exhaust_min_rpm, eff.intake_min_rpm,
              eff.air_assist_min_rpm);
        char lim[sizeof(pub_limits)];
        if (coolfmt_limits(lim, sizeof(lim), max_c, resume_c, temp_critical_c,
                           from, &eff) < 0)
            fflog(LOG_ERR, "cool: the effective limits do not fit their "
                  "status fragment; publishing none");
        pthread_mutex_lock(&mu);
        snprintf(pub_limits, sizeof(pub_limits), "%s", lim);
        pthread_mutex_unlock(&mu);
    }
    /* A header ceiling that does not tighten is named once per value:
     * the limits arrive with the job load, before the run session, and
     * leave with the job, so the value itself is the session. */
    if (h->coolant_max_c <= 0)
        looser_warned_c = -1.0;
    if (!gate_coolant_off && h->coolant_max_c > 0 && !from &&
        h->coolant_max_c != looser_warned_c && h->coolant_max_c >= temp_max_c) {
        looser_warned_c = h->coolant_max_c;
        fflog(LOG_INFO, "cool: header coolant ceiling %.1f C is not stricter "
              "than the local %.1f C; the local one stands",
              h->coolant_max_c, temp_max_c);
    }
}

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
    /* The tube's share of a heater rise depends on the power model: the
     * one the controller reports it is cutting with (an M101 can differ
     * from the default), else the configured default. */
    char model[16];
    int conf_density = !(settings_get("laser_power_model", model, sizeof(model)) == 0
                         && !strcmp(model, "analog"));
    laser_model_density = rep_model >= 0 ? rep_model : conf_density;
    gates_apply();
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
    /* snprintf reports the untruncated length; never publish more than
     * the buffer holds, and never a document whose closing brace was
     * cut off (the client refuses an incomplete verdict). */
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(body)) {
        fflog(LOG_ERR, "cool: verdict too long (%d bytes), "
                       "not published", n);
        return;
    }

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
    /* The baseline: the mean of the settled window the gate has just
     * verified when there is one, the first in-check sample otherwise. */
    uint32_t n = down_hist_n < FLOW_SETTLE_WIN ? down_hist_n : FLOW_SETTLE_WIN;
    if (n) {
        float sum = 0.0f;
        for (uint32_t i = 0; i < n; i++)
            sum += down_hist[(down_hist_n - n + i) % FLOW_SETTLE_WIN];
        flow_base_down = sum / (float)n;
        flow_base_set = 1;
    }
    flow_end_n = 0;
    /* Emission in the lag before the window reaches the sensor inside
     * it: the tube current of the last FLOW_LAG_S ticks opens the dose. */
    flow_hv_dose = 0.0;
    uint32_t h = hv_hist_n < FLOW_LAG_S ? hv_hist_n : FLOW_LAG_S;
    for (uint32_t i = 0; i < h; i++) {
        long v = hv_hist[(hv_hist_n - h + i) % FLOW_LAG_S];
        if (v > 0)
            flow_hv_dose += (double)v;
    }
    flow_laser_c = 0.0f;
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
        /* The run airflow shifts the coolant ADC by about a degree, so
         * the settled window a check's baseline comes from must be
         * taken entirely under the run profile: start the history
         * fresh, and the arm-time check waits FLOW_SETTLE_WIN ticks. */
        down_hist_n = 0;
        off_would_warned = 0;
        for (int i = 0; i < Fan_N; i++)
            airflow_reset(&fan_gate[i]);
        airflow_alarm = 0;
        fan_would_warned = 0;
        critical_alarm = 0;
        critical_would_warned = 0;
        job_temps_seen = 0;
        job_throttled = 0;
        fan_grace_until = now + (double)fan_grace_s;
        if (gate_flow_off)
            fflog(LOG_WARNING, "cool: coolant flow is not verified this "
                  "job (cool_flow_check_s = 0)");
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
        /* New run session: fresh lid-IR baseline and hv range, and a
         * standing fire alarm clears - a new job means the operator is
         * back at the machine. */
        for (int i = 0; i < 4; i++)
            ir_base[i] = -1;
        ir_over_ticks = 0;
        fire_alarm = 0;
        pgood_warned = 0;
        hv_lo = hv_hi = -1;
    } else if (cool_state == Cool_Run) {
        cool_state = Cool_Smoke;
        phase_until = now + (double)smoke_s;
        /* A fan fault is the session's: it ends with it, because the next
         * session re-proves every fan after the grace before anything can
         * fire. Left standing, the hold would cancel jogs at idle and the
         * cloud client's print pre-check would refuse the print that
         * re-proves the fan. */
        if (airflow_alarm)
            info("airflow fault cleared with the run session; the next "
                 "session judges every fan afresh");
        airflow_alarm = 0;
        for (int i = 0; i < Fan_N; i++)
            airflow_reset(&fan_gate[i]);
        /* The coolant critical fault likewise: the ceiling's pause tier
         * keeps holding while the loop is hot, and the next session
         * judges the critical line afresh. */
        if (critical_alarm) {
            info("coolant critical fault cleared with the run session; "
                 "the ceiling holds until the loop cools");
            critical_alarm = 0;
            /* The reason follows the verdict: the ceiling's hold, if it
             * stands, names itself again; otherwise nothing does. */
            if (over_temp_gate)
                overtemp_warn(0);
            else {
                pthread_mutex_lock(&mu);
                pub_reason[0] = '\0';
                pthread_mutex_unlock(&mu);
            }
        }
        critical_alarm = 0;
        flow_check_pending = 0;
        if (flow_check_active) {    /* run ended before the verdict */
            flow_check_active = 0;
            heater_set_pct(0);
        }
        if (job_temps_seen) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "temps this job: chassis %.1f..%.1f C, soc %.1f..%.1f C, "
                     "supply raw %ld..%ld%s",
                     job_chassis_lo, job_chassis_hi, job_soc_lo, job_soc_hi,
                     job_supply_lo, job_supply_hi,
                     job_throttled ? ", CPU THROTTLED" : "");
            if (job_throttled) {
                char more[48];
                snprintf(more, sizeof(more), " for %d s", job_throttled);
                strncat(msg, more, sizeof(msg) - strlen(msg) - 1);
            }
            info(msg);
        }
        /* The commissioning dataset: one line per job of what the fire
         * and HV sensors saw. */
        if (ir_base[0] >= 0) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "run telemetry: lid IR %ld/%ld/%ld/%ld peak "
                     "%ld/%ld/%ld/%ld, hv raw %ld..%ld",
                     ir_base[0], ir_base[1], ir_base[2], ir_base[3],
                     ir_peak[0], ir_peak[1], ir_peak[2], ir_peak[3],
                     hv_lo, hv_hi);
            info(msg);
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
    cool_limits_t hdr = rep_lim;
    pthread_mutex_unlock(&mu);

    int fresh = at >= 0 && now - at <= REPORT_TIMEOUT_S;
    limits_apply(&hdr, fresh);
    if (!fresh) {
        /* Dead-man for a HUNG controller, not just a dead one (the
         * supervisor covers deaths): a reporter that went silent with
         * the armed window open, or with the kernel still playing the
         * ring, has nobody left to stop the beam - stop motion and
         * lock the laser here. The armed case fires once per silence
         * episode; the running case repeats until the stop takes, so a
         * failed write cannot be a one-shot miss. Gated on at >= 0: a
         * freshly (re)started engine must not shoot down a healthy
         * orphaned controller that has not reported to it yet. */
        if (at >= 0 && ((armed && !silent_safed) || cnc_is_running())) {
            if (!silent_safed)
                warn("controller silent while armed/running - "
                     "stopping motion, locking laser");
            silent_safed = 1;
            wr_attr("cnc/stop", "1");
            wr_attr("cnc/laser_latch", "1");
        }
        mode = 0;
        armed = 0;
        if (at >= 0 && !silent_warned && (flood_on || cool_state == Cool_Run)) {
            silent_warned = 1;
            warn("controller went silent - standing down");
        }
    } else {
        silent_warned = 0;
        silent_safed = 0;
    }

    int flood = fresh && (mode == 1 || armed);
    int duty_changed = memcmp(run_duty, duty, sizeof(run_duty)) != 0;
    memcpy(run_duty, duty, sizeof(run_duty));
    int armed_rose = armed && !eff_armed;
    eff_armed = armed;
    /* Reapply for a duty change carried by a run report, or for the
     * armed window opening (armed, a job's profile may no longer hold a
     * fan below run duty). Only those: a run-ending report drops the
     * per-job profile with it, and writing the fallback duties from that
     * report would blast the fans on the way OUT of a job that ran a
     * quiet profile. A fan that just sped up gets its spin-up grace. */
    if ((duty_changed || armed_rose) && flood && flood_on &&
        cool_state == Cool_Run && !forced_cool) {
        long was[3] = {cmd_duty[0], cmd_duty[1], cmd_duty[2]};
        fans_run();     /* per-job profile changed mid-run */
        for (int i = 0; i < 3; i++)
            if (cmd_duty[i] > was[i]) {
                fan_grace_until = now + (double)fan_grace_s;
                break;
            }
    }

    if (flood != flood_on) {
        flood_on = flood;
        flood_apply(flood, now);
        if (flood) {
            /* The session start reloaded the settings: resolve the
             * effective limits from them now, so this tick's gate rows,
             * gates_off and the run-start log line agree. */
            limits_apply(&hdr, fresh);
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';   /* new session, stale reason gone */
            pthread_mutex_unlock(&mu);
        }
    }

    /* Emission witness: laser_on_sampled counts the ~1 s window's
     * emitting samples on the gated output of the hardware AND-gate -
     * physical evidence, not a commanded state. Emission with no armed
     * window in the recent past (3 s covers the sample-window lag plus
     * the job-end tail) gets the same treatment as a hung controller.
     * The stop repeats while the evidence persists. */
    if (fresh && armed)
        last_armed_at = now;
    long em = rd_long("cnc/laser_on_sampled");
    if (em > 0 && (last_armed_at < 0 || now - last_armed_at > 3.0)) {
        if (!emission_warned)
            warn("LASER EMISSION SENSED with no armed window - "
                 "stopping motion, locking laser");
        emission_warned = 1;
        wr_attr("cnc/stop", "1");
        wr_attr("cnc/laser_latch", "1");
    } else if (em == 0)
        emission_warned = 0;

    /* Power-good witness: warn once per session when the majority of
     * the sampled window read not-good while the window was armed. */
    if (fresh && armed && !pgood_warned) {
        long pg = rd_long("cnc/laser_pgood_sampled");
        if (pg >= 0 && pg < 128) {
            pgood_warned = 1;
            warn("laser power-good degraded during the armed window");
        }
    }

    /* Stepper-fault visibility: the kernel latches triggered faults;
     * surface a transition to nonzero during the run window. */
    long faults = rd_long("cnc/faults");
    if (faults > 0 && faults != last_faults && flood_on) {
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "stepper driver fault reported (mask %ld)", faults);
        warn(msg);
    }
    if (faults >= 0)
        last_faults = faults;

    /* Lid IR fire watch + HV range. Baseline is the first complete
     * reading of the run session; peaks build the commissioning
     * dataset; the abort gate only arms once cool_fire_ir_delta is
     * set from a characterized baseline. */
    long ir[4];
    int have_ir = 1;
    for (int i = 0; i < 4; i++) {
        char attr[20];
        snprintf(attr, sizeof(attr), "pic/lid_ir_%d", i + 1);
        ir[i] = rd_long(attr);
        if (ir[i] < 0)
            have_ir = 0;
    }
    if (have_ir && cool_state == Cool_Run) {
        if (ir_base[0] < 0) {
            for (int i = 0; i < 4; i++)
                ir_base[i] = ir_peak[i] = ir[i];
        } else {
            int over = 0;
            for (int i = 0; i < 4; i++) {
                if (ir[i] > ir_peak[i])
                    ir_peak[i] = ir[i];
                if (fire_ir_delta > 0 &&
                    ir[i] - ir_base[i] > (long)fire_ir_delta)
                    over = 1;
            }
            ir_over_ticks = over ? ir_over_ticks + 1 : 0;
            if (ir_over_ticks >= FIRE_IR_TICKS && !fire_alarm) {
                fire_alarm = 1;
                warn("LID IR FIRE SIGNAL - motion stopped, laser "
                     "locked, smoke airflow held");
                wr_attr("cnc/stop", "1");
                wr_attr("cnc/laser_latch", "1");
                fans_run();     /* full smoke-clear airflow */
            }
        }
    }
    if (cool_state == Cool_Run) {
        long hv = rd_long("pic/hv_current");
        hv_last = hv;
        if (hv >= 0) {
            if (hv_lo < 0 || hv < hv_lo)
                hv_lo = hv;
            if (hv > hv_hi)
                hv_hi = hv;
        }
    } else
        hv_last = -1;
    /* The tube current of the last FLOW_LAG_S ticks, for a flow check
     * that starts inside a lit run (flow_check_start). */
    hv_hist[hv_hist_n % FLOW_LAG_S] = hv_last;
    hv_hist_n++;
    pthread_mutex_lock(&mu);
    pub_fire_watch = fire_alarm ? "ALARM"
                   : fire_ir_delta > 0 ? "armed" : "watch";
    pthread_mutex_unlock(&mu);

    /* The airflow gates: a fan is judged while the run profile is
     * applied and the laser is armed, or the fan is commanded at the run
     * duty its floor was measured at (airflow_judged); a job that runs a
     * fan slower unarmed (a hunt, extraction fans off) is measured and
     * published, not judged. Nothing counts during the spin-up grace.
     * The exhaust and the intakes report a period in ns at 2 pulses/rev,
     * the air assist in us at 8, the purge fan a current (no duty: it is
     * always on, so always judged in run). The floors are the effective
     * ones (a header can raise a tach floor for a job). A trip is a
     * fault for the rest of the session: hold, fire blocked, no resume;
     * the fans stay at run duty, which is what a stalled extraction fan
     * needs around it. An off gate (floor 0) still measures and names
     * the first reading that would have tripped the shipped default. */
    {
        fan_reading[Fan_Exhaust] = airflow_rpm(rd_long("thermal/tach_exhaust"), 1e9, 2);
        fan_reading[Fan_Intake1] = airflow_rpm(rd_long("thermal/tach_intake_1"), 1e9, 2);
        fan_reading[Fan_Intake2] = airflow_rpm(rd_long("thermal/tach_intake_2"), 1e9, 2);
        fan_reading[Fan_AirAssist] = airflow_rpm(rd_long("head/air_assist_tach"), 1e6, 8);
        long purge = rd_long("head/purge_air_current");
        fan_reading[Fan_Purge] = purge > 0 ? (double)purge : 0.0;
        fan_floor[Fan_Exhaust] = eff_lim.exhaust_min_rpm;
        fan_floor[Fan_Intake1] = eff_lim.intake_min_rpm;
        fan_floor[Fan_Intake2] = eff_lim.intake_min_rpm;
        fan_floor[Fan_AirAssist] = eff_lim.air_assist_min_rpm;
        fan_floor[Fan_Purge] = purge_min_current;
        static const double fan_default[Fan_N] = {
            TACH_EXHAUST_MIN_RPM, TACH_INTAKE_MIN_RPM, TACH_INTAKE_MIN_RPM,
            TACH_AIR_ASSIST_MIN_RPM, PURGE_MIN_CURRENT };
        static const int fan_duty_ix[Fan_N] = {1, 2, 2, 0, -1};
        static const long fan_local[3] = {AIR_ASSIST_RUN, EXHAUST_RUN, INTAKE_RUN};
        int run = cool_state == Cool_Run;
        int in_grace = now < fan_grace_until;
        coolfmt_fan_t rows[Fan_N];
        for (int i = 0; i < Fan_N; i++) {
            airflow_state_t st;
            int dx = fan_duty_ix[i];
            int judged = dx < 0 ? airflow_judged(run, armed, 1, 1)
                       : airflow_judged(run, armed, cmd_duty[dx], fan_local[dx]);
            if (judged) {
                st = airflow_tick(&fan_gate[i], fan_reading[i], fan_floor[i], in_grace);
                if (st == Air_Tripped && !airflow_alarm) {
                    airflow_alarm = 1;
                    char msg[96];
                    snprintf(msg, sizeof(msg),
                             "AIRFLOW: %s %.0f under the %.0f floor for %d s - "
                             "hold, no resume this job",
                             fan_name[i], fan_reading[i], fan_floor[i],
                             AIRFLOW_DEBOUNCE_TICKS);
                    warn(msg);
                }
                if (st == Air_Off && !in_grace && !fan_would_warned &&
                    fan_reading[i] < fan_default[i]) {
                    fan_would_warned = 1;
                    fflog(LOG_WARNING, "cool: gate %s is OFF and would have "
                          "tripped at the default %.0f (reading %.0f)",
                          fan_name[i], fan_default[i], fan_reading[i]);
                }
            } else {
                st = fan_gate[i].tripped ? Air_Tripped
                   : fan_floor[i] <= 0.0 ? Air_Off : Air_Ok;
            }
            rows[i].name = fan_name[i];
            rows[i].reading = fan_reading[i];
            rows[i].floor = fan_floor[i];
            rows[i].state = judged ? airflow_state_name(st)
                          : st == Air_Tripped ? "TRIPPED"
                          : st == Air_Off ? "off"
                          : run ? "unjudged" : "idle";
        }
        char fg[sizeof(pub_fan_gates)];
        if (coolfmt_fan_gates(fg, sizeof(fg), rows, Fan_N) < 0)
            fflog(LOG_ERR, "cool: the fan gate rows do not fit their status "
                  "fragment; publishing none");
        pthread_mutex_lock(&mu);
        snprintf(pub_fan_gates, sizeof(pub_fan_gates), "%s", fg);
        pthread_mutex_unlock(&mu);
    }

    float down = 0, up = 0;
    int have_down = read_temp("pic/water_temp_1", &down);
    int have_up = read_temp("pic/water_temp_2", &up);

    if (have_up && gate_coolant_off) {
        /* The ceiling is at its off end: no gate, and no hold from it.
         * The reading is still watched against the shipped default so
         * a machine running without the gate leaves a record of what
         * it would have done. */
        if (over_temp_gate) {
            over_temp_gate = 0;
            pthread_mutex_lock(&mu);
            pub_reason[0] = '\0';
            pthread_mutex_unlock(&mu);
            if (forced_cool) {
                forced_cool = 0;
                fans_apply_phase();
            }
        }
        if (up > TEMP_MAX_C_DEFAULT && !off_would_warned) {
            off_would_warned = 1;
            fflog(LOG_WARNING, "cool: gate coolant_max is OFF and would "
                  "have tripped at the default %.0f C (coolant %.1f C)",
                  TEMP_MAX_C_DEFAULT, up);
        }
    } else if (have_up) {
        /* Fire gate with hysteresis: gated over the run ceiling, back
         * in service under the resume gate. The hold request rides the
         * same gate: the controller holds while it stands, resumes
         * (its call) once resume_ok returns. */
        int was = over_temp_gate;
        over_temp_gate = up > (over_temp_gate ? eff_temp_resume_c : eff_temp_max_c);
        if (over_temp_gate && !was) {
            last_up_c = up;
            overtemp_warn(1);
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

    /* The SoC's own thermal governor: it throttles the CPU at its
     * passive trip (85 C on this part) and powers the board off at its
     * critical one (90 C), with no help from here. A throttle is named
     * when it starts and when it ends, since a slower CPU reaches the
     * engine, the camera and the protocol thread before it reaches the
     * step stream (which has the ring in hand). */
    {
        long thr = soc_throttle_state();
        if (thr > 0 && throttle_state <= 0) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "SoC throttled: CPU cooling state %ld (die %.1f C)",
                     thr, soc_degc());
            fflog(LOG_WARNING, "cool: %s", msg);
        } else if (thr == 0 && throttle_state > 0) {
            char msg[64];
            snprintf(msg, sizeof(msg),
                     "SoC throttle released (die %.1f C)", soc_degc());
            info(msg);
        }
        throttle_state = thr;
        if (cool_state == Cool_Run && thr > 0)
            job_throttled++;
    }

    /* The board temperatures over the session, for the run-end line. */
    if (cool_state == Cool_Run) {
        double ch = chassis_degc();
        double soc = soc_degc();
        long sp = supply_temp_raw();
        if (ch > -100 && sp >= 0) {
            if (soc <= -100)
                soc = job_temps_seen ? job_soc_hi : -273.15;
            if (!job_temps_seen) {
                job_chassis_lo = job_chassis_hi = ch;
                job_soc_lo = job_soc_hi = soc;
                job_supply_lo = job_supply_hi = sp;
                job_temps_seen = 1;
            } else {
                if (ch < job_chassis_lo) job_chassis_lo = ch;
                if (ch > job_chassis_hi) job_chassis_hi = ch;
                if (soc < job_soc_lo) job_soc_lo = soc;
                if (soc > job_soc_hi) job_soc_hi = soc;
                if (sp < job_supply_lo) job_supply_lo = sp;
                if (sp > job_supply_hi) job_supply_hi = sp;
            }
        }
    }

    /* The critical tier: the upstream coolant at or over
     * cool_temp_critical_c during a run session is a fault, not a pause
     * (CRITICAL: hold, fire blocked, no resume this session; the session
     * ends it, like a fan fault, and the ceiling's pause tier keeps the
     * hold while the loop is hot). Local only: no header carries a
     * critical line for the coolant. At its far end the gate is off and
     * still measures against the shipped default. */
    if (have_up && cool_state == Cool_Run) {
        if (gate_critical_off) {
            if (up >= TEMP_CRITICAL_C_DEFAULT && !critical_would_warned) {
                critical_would_warned = 1;
                fflog(LOG_WARNING, "cool: gate coolant_critical is OFF and "
                      "would have tripped at the default %.0f C (coolant "
                      "%.1f C)", TEMP_CRITICAL_C_DEFAULT, up);
            }
        } else if (!critical_alarm && up >= temp_critical_c) {
            critical_alarm = 1;
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "CRITICAL: coolant %.1f C at or over the %.0f C critical "
                     "line - hold, no resume this job", up, temp_critical_c);
            warn(msg);
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
        flow_end_hist[flow_end_n % FLOW_END_WIN] = down;
        flow_end_n++;
        /* The tube's heat lags the sensor by FLOW_LAG_S: emission in the
         * window's last FLOW_LAG_S seconds has not arrived by its end. */
        if (hv_last > 0 && now < flow_check_end - (double)FLOW_LAG_S)
            flow_hv_dose += (double)hv_last;
        if (now >= flow_establish_at) {
            flow_dt_sum += down - up;
            flow_dt_n++;
        }
        if (now >= flow_check_end) {
            uint32_t n_end = flow_end_n < FLOW_END_WIN ? flow_end_n : FLOW_END_WIN;
            float end_sum = 0.0f;
            for (uint32_t i = 0; i < n_end; i++)
                end_sum += flow_end_hist[i];
            float rise_raw = end_sum / (float)n_end - flow_base_down;
            float k = laser_model_density ? laser_heat_density : laser_heat_cw;
            flow_laser_c = (float)(flow_hv_dose * (double)k);
            if (flow_laser_c < 0.0f)
                flow_laser_c = 0.0f;
            if (flow_laser_c > LASER_HEAT_MAX_C)
                flow_laser_c = LASER_HEAT_MAX_C;
            float rise = rise_raw - flow_laser_c;
            float dt = flow_dt_n ? flow_dt_sum / (float)flow_dt_n : 0.0f;
            char msg[112], laser[32] = "";

            if (flow_laser_c >= 0.05f)
                snprintf(laser, sizeof(laser), "; laser %.1f off %.1f",
                         flow_laser_c, rise_raw);
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
                             "COOLANT FLOW SUSPECT: heater rise %.1f C (limit %.1f, dT %.1f%s) - re-checking",
                             rise, flow_fault_rise, dt, laser);
                } else {
                    flow_verdict = Flow_Fault;
                    snprintf(msg, sizeof(msg),
                             "COOLANT FLOW FAULT: heater rise %.1f C (limit %.1f, dT %.1f%s) - check the pump",
                             rise, flow_fault_rise, dt, laser);
                }
                warn(msg);
            } else if (flow_verdict != Flow_Normal) {
                snprintf(msg, sizeof(msg),
                         flow_verdict == Flow_Suspect
                           ? "coolant flow suspicion cleared (heater rise %.1f C, dT %.1f C%s)"
                           : "coolant flow recovered (heater rise %.1f C, dT %.1f C%s)",
                         rise, dt, laser);
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
                         "coolant flow verified (heater rise %.1f C, dT %.1f C%s)",
                         rise, dt, laser);
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

    /* Cooldown phases. The drop to idle duty is additionally gated on
     * the kernel actually being done: the ring can still be playing
     * (cloud mode preloads whole jobs) after the report-driven phases
     * expire, and airflow must never fall below cooldown duty while
     * the machine can still be depositing energy. */
    if (cool_state == Cool_Smoke && now >= phase_until) {
        if ((have_up && up > temp_resume_c) || cnc_is_running()) {
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
        if ((recovered || now >= phase_until) && cnc_is_running()) {
            phase_until = now + (double)cooldown_max_s;  /* ring still live */
        } else if (recovered || now >= phase_until) {
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
    const char *verdict = fire_alarm ? "FIRE"
                        : airflow_alarm ? "AIRFLOW"
                        : critical_alarm ? "CRITICAL"
                        : over_temp_gate ? "OVERTEMP"
                        : flow_verdict == Flow_Fault ? "FAULT"
                        : flow_verdict == Flow_Suspect ? "SUSPECT" : "OK";
    int hold = fire_alarm || airflow_alarm || critical_alarm || over_temp_gate
             || (armed && flow_verdict != Flow_Normal);
    int fire_ok = fresh && !fire_alarm && !airflow_alarm && !critical_alarm
                && !over_temp_gate && flow_verdict != Flow_Fault;

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
        fflog(LOG_ERR, "cool: engine thread failed to start");
        return;
    }
    fflog(LOG_INFO, "cool: engine started");
}

void cool_shutdown(void)
{
    pthread_mutex_lock(&mu);
    int was = engine_run;
    engine_run = 0;
    pthread_mutex_unlock(&mu);
    if (was)
        pthread_join(engine_th, NULL);
    /* The heater is this engine's own flow-check heat source; off is
     * always the right parting state, job or no job. */
    heater_set_pct(0);
    /* A busy machine keeps its orphaned controller running (see
     * super_shutdown), so idling the fans here would drop exhaust under
     * a live cut, and unlinking the verdict would feed-hold it the
     * moment a reader notices. Leave both alone: an aging verdict file
     * reads exactly like a stopped engine within the 2 s staleness
     * window, and the restarted daemon reasserts the right posture. */
    if (machine_is_idle()) {
        fans_idle();
        unlink(VERDICT_FILE);   /* missing file = fire blocked at readers */
    }
}

void cool_state_model(int density)
{
    rep_model = density < 0 ? -1 : !!density;   /* one aligned int: atomic on this core */
}

int cool_state_report(const char *mode, int armed,
                      long air_assist, long exhaust, long intake,
                      const cool_limits_t *lim)
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
    if (lim)
        rep_lim = *lim;
    else {
        cool_limits_t none = {-1, -1, -1, -1, -1};
        rep_lim = none;
    }
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
    coolfmt_status_t st = {
        .phase = pub_phase, .verdict = pub_verdict, .reason = pub_reason,
        .fire_watch = pub_fire_watch, .fire_ok = pub_fire_ok, .hold = pub_hold,
        .armed = rep_armed, .down_c = pub_down, .up_c = pub_up,
        .report_age_s = rep_at < 0 ? -1 : wall_s() - rep_at,
        .gates_off = pub_gates_off, .limits = pub_limits,
        .fan_gates = pub_fan_gates,
    };
    int rc = coolfmt_status(buf, len, &st);
    pthread_mutex_unlock(&mu);
    return rc;
}

int cool_gates_off_json(char *buf, size_t len)
{
    pthread_mutex_lock(&mu);
    snprintf(buf, len, "%s", pub_gates_off);
    pthread_mutex_unlock(&mu);
    return 0;
}

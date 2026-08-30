/*
 * cool_flow_test.c - host unit test for the flow check's reading
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The flow check heats the loop for a window and judges the downstream
 * rise. Two things the bench found put an ordinary job's check within
 * tenths of the limit: the coolant ADC's common-mode offset (about 1 C,
 * stepping in with the run airflow and toggling between two levels), and
 * the tube's own heat (about 1.5 C over a lit CW window). The engine now
 * reads the baseline and the end as means and takes the tube's share off
 * the rise, bounded. This test drives the engine tick by tick against a
 * fake sysfs tree and a fake clock, with a loop model that carries the
 * offset, the heater's rise (flow or no flow) and the tube's lagged heat:
 *
 *   A. dark, flow:          verified, no share taken
 *   B. dark, no flow:       SUSPECT
 *   C. lit CW, flow:        verified, the share about 1.5 C
 *   D. lit CW, no flow:     SUSPECT (the share never masks a stopped pump)
 *   E. absurd coefficient:  the share is bounded, no flow still SUSPECT
 *   F. density model:       the density coefficient applies (0.77 of CW)
 */
#define GF_SYSFS    "cool-flow-test/sys/"
#define VERDICT_DIR "cool-flow-test/run"
#define clock_gettime fake_clock_gettime

#include "../src/cool.c"

#include <stdarg.h>
#include <sys/stat.h>

/* --- fakes ------------------------------------------------------------ */

static double fake_now = 1000.0;

int fake_clock_gettime(clockid_t id, struct timespec *ts)
{
    (void)id;
    ts->tv_sec = (time_t)fake_now;
    ts->tv_nsec = (long)((fake_now - (double)(time_t)fake_now) * 1e9);
    return 0;
}

static const char *kv[16];          /* key, value, ..., NULL */

int settings_get(const char *key, char *val, size_t len)
{
    for (size_t i = 0; kv[i]; i += 2)
        if (!strcmp(kv[i], key)) {
            snprintf(val, len, "%s", kv[i + 1]);
            return 0;
        }
    return -1;
}

static char last_log[256];

void fflog(int prio, const char *fmt, ...)
{
    (void)prio;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_log, sizeof(last_log), fmt, ap);
    va_end(ap);
    printf("    log: %s\n", last_log);
}

int diag_running(void) { return 0; }
int status_json(char *buf, size_t len) { (void)buf; (void)len; return 0; }
int machine_is_idle(void) { return 1; }
double chassis_degc(void) { return 25.0; }
long supply_temp_raw(void) { return 540; }
double soc_degc(void) { return 38.0; }
long soc_throttle_state(void) { return 0; }

/* The factory conversion (status.c), and its inverse for the model. */
double coolant_degc(long raw)
{
    static const double adc_f = 1024.0 * 1.3;
    if (raw <= 0 || (double)raw >= adc_f)
        return -273.15;
    double r = 10000.0 / (adc_f / (double)raw - 1.0);
    double rinf = 10000.0 * exp(-3380.0 / 298.15);
    return 3380.0 / log(r / rinf) - 273.15;
}

static long raw_for(double c)
{
    double rinf = 10000.0 * exp(-3380.0 / 298.15);
    double r = rinf * exp(3380.0 / (c + 273.15));
    return (long)(1024.0 * 1.3 / (1.0 + 10000.0 / r) + 0.5);
}

/* --- the fake tree ---------------------------------------------------- */

static void put(const char *attr, const char *v)
{
    char path[160];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(v, f);
        fclose(f);
    }
}

static void put_long(const char *attr, long v)
{
    char s[24];
    snprintf(s, sizeof(s), "%ld\n", v);
    put(attr, s);
}

static long get_long(const char *attr)
{
    char path[160], buf[24];
    snprintf(path, sizeof(path), GF_SYSFS "%s", attr);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long v = fgets(buf, sizeof(buf), f) ? strtol(buf, NULL, 10) : -1;
    fclose(f);
    return v;
}

static void make_tree(void)
{
    mkdir("cool-flow-test", 0755);
    mkdir("cool-flow-test/run", 0755);
    mkdir(GF_SYSFS, 0755);
    mkdir(GF_SYSFS "pic", 0755);
    mkdir(GF_SYSFS "thermal", 0755);
    mkdir(GF_SYSFS "head", 0755);
    mkdir(GF_SYSFS "cnc", 0755);
    put("cnc/state", "idle\n");
    put_long("cnc/faults", 0);
    put_long("cnc/laser_on_sampled", 0);
    put_long("cnc/laser_pgood_sampled", 0);
    /* Tach counts are periods: a small count is a fast fan. */
    put_long("thermal/tach_exhaust", 300);
    put_long("thermal/tach_intake_1", 300);
    put_long("thermal/tach_intake_2", 300);
    put_long("head/air_assist_tach", 300);
    put_long("head/purge_air_current", 629);
    put_long("thermal/heater_pwm", 0);
    put_long("pic/hv_current", 0);
}

/* --- the loop model --------------------------------------------------- */

static const double BASE_C = 22.3;

typedef struct {
    int flow;                       /* pump running */
    int lit;                        /* tube at full current from the open */
    double k_true;                  /* the tube's real C per raw-second */
    int session;                    /* ticks since the session opened */
    int heater_ticks;               /* ticks the heater has been on */
    double tube_c;                  /* tube heat arrived at the sensor */
} loop_t;

static void loop_tick(loop_t *L)
{
    /* The engine's own view of the heater: the fake tree is not sysfs,
       and a short write does not truncate what a longer one left. */
    long heater = flow_check_active ? 1 : 0;
    if (heater > 0)
        L->heater_ticks++;
    else
        L->heater_ticks = 0;
    double th = L->heater_ticks ? (double)(L->heater_ticks - 1) : 0.0;
    double heater_c = 0.0;
    if (L->heater_ticks)
        heater_c = L->flow ? 12.0 * (1.0 - exp(-th / 12.0)) : 0.4 * th;
    /* The tube's heat arrives 15 s after emission. */
    long hv = L->lit && L->session > 0 ? 970 : 0;
    if (L->lit && L->session > 15)
        L->tube_c += L->k_true * 970.0;
    /* The run airflow's ADC offset: in from the session open, toggling. */
    double offset = L->session > 0 ? -1.0 + ((L->session & 1) ? 0.6 : 0.0) : 0.0;
    double down = BASE_C + offset + heater_c + L->tube_c;
    double up = BASE_C + offset + 0.07 * heater_c + L->tube_c;
    put_long("pic/water_temp_1", raw_for(down));
    put_long("pic/water_temp_2", raw_for(up));
    put_long("pic/hv_current", hv);
    if (L->session > 0)
        L->session++;
}

/* --- driving the engine ----------------------------------------------- */

/* Close the session the way a job's end does: report idle and tick
   through the cooldown phases until the engine is idle with the heater
   off, then clear the verdict a previous case left. */
static void close_session(loop_t *L)
{
    L->session = 0;
    L->lit = 0;
    for (int i = 0; i < 900; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
        if (cool_state == Cool_Idle && !flow_check_active)
            break;
    }
    flow_verdict = Flow_Normal;
    flow_episodes = 0;
    flow_check_pending = 0;
    heater_set_pct(0);
    conf_reload();
}

/* Open a run session and tick until the check has run and judged.
   Returns the verdict; the engine's line is in last_log. */
static flow_verdict_t run_check(loop_t *L, int lit, int max_ticks)
{
    close_session(L);
    L->lit = lit;
    L->heater_ticks = 0;
    L->tube_c = 0.0;
    /* A settled loop before the job: history for the gate. */
    for (int i = 0; i < 20; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("idle", 0, -1, -1, -1, NULL);
        engine_tick();
    }
    L->session = 1;
    int saw_check = 0;
    for (int i = 0; i < max_ticks; i++) {
        loop_tick(L);
        fake_now += 1.0;
        cool_state_report("run", 1, -1, -1, -1, NULL);
        engine_tick();
        if (getenv("DEBUG") && i % 5 == 0)
            printf("    t=%3d state=%d pending=%d active=%d hist=%u heater=%ld "
                   "down=%.2f up=%.2f verdict=%d\n", i, (int)cool_state,
                   flow_check_pending, flow_check_active, (unsigned)down_hist_n,
                   get_long("thermal/heater_pwm"),
                   coolant_degc(get_long("pic/water_temp_1")),
                   coolant_degc(get_long("pic/water_temp_2")), (int)flow_verdict);
        if (flow_check_active)
            saw_check = 1;
        else if (saw_check)
            return flow_verdict;
    }
    printf("    (the check never completed in %d ticks; active=%d pending=%d)\n",
           max_ticks, flow_check_active, flow_check_pending);
    return Flow_Fault;
}

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    make_tree();
    /* The coefficient is selected by the model the controller reports
     * (the config key is gone; density is the only product model, and
     * the analog value survives for the host-test reference builds). */
    cool_state_model(0);
    kv[0] = NULL;
    loop_t L = { .flow = 1, .lit = 0, .k_true = LASER_HEAT_CW };

    printf("A. dark, flow\n");
    flow_verdict_t v = run_check(&L, 0, 200);
    CHECK(v == Flow_Normal, "a dark check with flow is verified");
    CHECK(strstr(last_log, "verified") != NULL, "the line says verified");
    CHECK(strstr(last_log, "laser") == NULL, "no share is taken with the tube dark");
    CHECK(flow_base_set && fabs(flow_base_down - (BASE_C - 0.7)) < 0.15,
          "the baseline is the mean of the settled window, not one sample");

    printf("B. dark, no flow\n");
    L.flow = 0;
    v = run_check(&L, 0, 200);
    CHECK(v == Flow_Suspect, "a dark check without flow is SUSPECT");
    CHECK(strstr(last_log, "SUSPECT") != NULL, "the line says SUSPECT");

    printf("C. lit CW from the session open, flow\n");
    L.flow = 1;
    v = run_check(&L, 1, 200);
    CHECK(v == Flow_Normal, "a lit check with flow is verified");
    CHECK(flow_laser_c > 1.2f && flow_laser_c < 1.8f,
          "the tube's share taken off is about 1.5 C");
    CHECK(strstr(last_log, "laser") != NULL, "the line carries the share");

    printf("D. lit CW, no flow\n");
    L.flow = 0;
    v = run_check(&L, 1, 200);
    CHECK(v == Flow_Suspect, "a lit check without flow is still SUSPECT");

    printf("E. an absurd coefficient cannot subtract the check away\n");
    kv[0] = "cool_laser_heat_cw"; kv[1] = "1e-3"; kv[2] = NULL;
    v = run_check(&L, 1, 200);
    CHECK(flow_laser_c <= LASER_HEAT_MAX_C + 0.001f, "the share is bounded");
    CHECK(v == Flow_Suspect, "no flow is still SUSPECT under the bound");
    kv[0] = NULL;

    printf("F. the density model takes its own coefficient\n");
    cool_state_model(1);
    L.flow = 1;
    L.k_true = LASER_HEAT_DENSITY;
    v = run_check(&L, 1, 200);
    CHECK(v == Flow_Normal, "a lit density check with flow is verified");
    CHECK(flow_laser_c > 0.9f && flow_laser_c < 1.4f,
          "the share is the density coefficient's (0.77 of CW)");

    printf("G. the air-assist offset is taken off in counts while the fan runs\n");
    /* The fan's ground lift raises the raw counts, and more counts read
       colder. The model writes raw counts with no such lift in them, so
       with the setting at 20 the engine must read both sensors 20 counts
       LOWER than the raw (warmer) while the run profile is on, and the raw
       when it is not. The flow check must not care (both ends shift
       alike). */
    kv[0] = "cool_aa_offset_counts"; kv[1] = "20"; kv[2] = NULL;
    L.flow = 1;
    L.lit = 0;
    v = run_check(&L, 0, 200);
    CHECK(v == Flow_Normal, "the check still verifies flow with the offset setting in force");
    CHECK(cool_coolant_offset_counts() == 20, "the run profile commands the fan: 20 counts added");
    {
        long raw = get_long("pic/water_temp_2");            /* the model's last raw upstream */
        float engine_up = pub_up;
        float expect = (float)coolant_degc(raw - 20);
        CHECK(fabs(engine_up - expect) < 0.05,
              "the published upstream reading is the raw less 20 counts");
        CHECK(engine_up > (float)coolant_degc(raw) + 0.8,
              "the correction is worth about a degree at this temperature");
    }
    close_session(&L);
    CHECK(cool_coolant_offset_counts() == 0, "the fan idle: no counts added");
    kv[0] = "cool_aa_offset_counts"; kv[1] = "500"; kv[2] = NULL;
    conf_reload();
    aa_write(1023);
    CHECK(cool_coolant_offset_counts() == 60, "an absurd setting is bounded at 60 counts");
    aa_write(600);
    CHECK(cool_coolant_offset_counts() > 20 && cool_coolant_offset_counts() < 40,
          "a part duty adds part of the setting (600 of 1023)");
    aa_write(204);
    kv[0] = NULL;
    conf_reload();

    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the flow check reads means, takes the tube's share off, "
                      "bounds it, still sees a stopped pump under a lit tube, and takes "
                      "the air-assist offset off while the fan runs\n",
           failures);
    return failures ? 1 : 0;
}

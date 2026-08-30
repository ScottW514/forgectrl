/*
 * cool.h - cooling engine (see cool.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_COOL_H
#define FORGECTRL_COOL_H

#include <stddef.h>

/* Flow-check parameters, shared between the engine (cool.c) and the
 * diagnostics runner (diag.c) so the tools always test the check the
 * engine actually runs. The measured rationale for every value is in
 * cool.c. Configured cool_* keys override the defaults at run time in
 * both consumers. */
#define COOL_FLOW_RISE_C     14.4f  /* fault threshold: downstream rise */
#define COOL_FLOW_HEATER_PCT 40     /* check heater duty */
#define COOL_FLOW_CHECK_S    50     /* check window; 0 disables */
/* Settle gate: baseline capture only from a stationary loop. */
#define COOL_SETTLE_DT_C     1.5f   /* |downstream - upstream| */
#define COOL_SETTLE_DRIFT_C  0.4f   /* split-half mean difference */
#define COOL_SETTLE_WIN      15     /* 1 Hz samples */
/* Factory run fan duties (pulse-header ground truth; also the
 * controllers' compiled-in emergency-fallback values per the
 * contract). */
#define COOL_AIR_ASSIST_RUN  1023
#define COOL_EXHAUST_RUN     65535
#define COOL_INTAKE_RUN      43278

/* Start the engine: resolve tunables (GFCOOL_* env > cool_* settings >
 * compiled defaults), take the idle posture (pump on, TEC off, purge
 * on, heater off, fans idle), and launch the 1 Hz policy thread that
 * writes /run/forgefirm/cooling.state. */
void cool_init(void);

/* Stop the thread, stand the hardware down to the idle posture and
 * remove the verdict file (readers treat a missing file as
 * fire-blocked). */
void cool_shutdown(void);

/* Per-job limits a report may carry (cloud mode passes the pulse
 * header's envelope; GRBL mode passes none). Each is a value the
 * engine applies only where it is stricter than its own configured
 * limit; a field at or below zero is absent. */
typedef struct {
    double coolant_max_c;       /* run ceiling, upstream sensor */
    double coolant_min_c;       /* run floor (no gate yet; logged) */
    double exhaust_min_rpm;     /* fan floors (no gate yet; logged) */
    double intake_min_rpm;
    double air_assist_min_rpm;
} cool_limits_t;

/* Job-state report from the active controller (POST /cool/state).
 * mode is "idle", "run" or "cooldown"; armed = laser armed (forces the
 * run profile and flow interrogation whatever mode says). The duty
 * arguments override the run fan profile for this job; pass -1 to use
 * the configured/factory values. lim carries the job's limits, or
 * NULL for none. Returns 0, or -1 on a bad mode. */
int cool_state_report(const char *mode, int armed,
                      long air_assist, long exhaust, long intake,
                      const cool_limits_t *lim);

/* The dose model the controller reports it is cutting with: 1 density,
 * 0 analog, -1 not said (the laser_power_model key applies). Read when a
 * run's tunables load, so the tube-heat share follows the model in force
 * rather than the configured default an M101 may have overridden. */
void cool_state_model(int density);

/* Engine state as JSON (verdict, temps, phase, last report age, the
 * effective limits and the per-fan gate states) for the UI and bench
 * tooling. A buffer of COOL_STATUS_JSON_MAX bytes always holds the
 * whole document (coolfmt.h sizes the fragments, and a fragment that
 * does not fit is published empty, never cut); returns -1 when the
 * buffer given was too small, so a caller never serves a cut-off
 * document as JSON. */
#define COOL_STATUS_JSON_MAX 1536
int cool_status_json(char *buf, size_t len);

/* Seconds since the last job-state report, or -1 if none has ever
 * arrived (the supervisor uses this to see a controller come alive). */
double cool_report_age(void);

/* The counts the air-assist fan's ground shift adds to a raw coolant
 * thermistor reading at the duty the engine last commanded (more counts
 * read colder): cool_aa_offset_counts scaled by the fan's current above
 * its start, bounded; zero when the setting is zero or the fan is idle.
 * Every consumer of the two readings takes it off before the conversion. */
long cool_coolant_offset_counts(void);

/* The gates whose setting sits at its off end, as a JSON array of gate
 * names ("[]" when every gate is on), from the engine's resolved
 * tunables at the last run start. /status carries it beside the
 * settings reply's per-key state. */
int cool_gates_off_json(char *buf, size_t len);

#endif

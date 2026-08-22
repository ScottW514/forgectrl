/*
 * airflow.h - the per-fan airflow gate: floor, grace, debounce, latch
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One gate per fan the engine drives at run duty: the exhaust, the two
 * intakes and the air assist by tachometer, the purge-air fan by its
 * current. Each compares a 1 Hz reading against a floor. A fan needs time
 * to reach speed after the run profile is written, so nothing counts
 * during a grace window; a tach reading wanders, so a trip takes three
 * consecutive ticks under the floor and a good tick clears the count; a
 * trip is a fault, latched until the next run session, because a fan
 * that is not moving the air is not a condition a job resumes through.
 * A floor at or below zero is the gate turned off by value (gates.c):
 * nothing counts and nothing latches.
 *
 * The floors were measured with each fan at the configured run duty, so
 * a gate judges a fan only at that operating point: while the laser is
 * armed (when a job's own profile may raise a fan but never lower it),
 * or whenever the fan is commanded at or above the run duty. A job that
 * runs its fans slower unarmed (a hunt, with the extraction fans off)
 * is measured and published, not judged.
 *
 * Pure: the engine feeds readings, the host test feeds numbers.
 */
#ifndef FORGECTRL_AIRFLOW_H
#define FORGECTRL_AIRFLOW_H

typedef enum {
    Air_Off = 0,     /* floor <= 0: no gate */
    Air_Grace,       /* spin-up window: not counted */
    Air_Ok,          /* at or above the floor */
    Air_Under,       /* under the floor, not yet for the debounce */
    Air_Tripped      /* latched until airflow_reset() */
} airflow_state_t;

#define AIRFLOW_DEBOUNCE_TICKS 3

typedef struct {
    int under_ticks;
    int tripped;
    airflow_state_t state;
} airflow_fan_t;

/* One tick. Returns the state after it. */
airflow_state_t airflow_tick(airflow_fan_t *f, double reading, double floor, int in_grace);

/* A new run session: counters and the latch cleared. */
void airflow_reset(airflow_fan_t *f);

const char *airflow_state_name(airflow_state_t s);

/* Speed from a tach period: the kernel reports the period between
 * pulses (0 = stopped or absent). */
double airflow_rpm(long period, double units_per_second, int pulses_per_rev);

/* The duty a fan is written at for a run session. job is the job's own
 * profile (the pulse header in cloud mode), negative for none. Armed,
 * the job may only raise the fan above the configured run duty, never
 * lower it: the floor was measured there. Unarmed, the job's duty
 * stands. */
long airflow_run_duty(long local, long job, int armed);

/* Whether a fan's gate judges this tick: the run profile is applied,
 * and the laser is armed or the fan is commanded at or above the run
 * duty its floor was measured at. */
int airflow_judged(int run, int armed, long duty, long local);

#endif

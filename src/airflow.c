/*
 * airflow.c - the per-fan airflow gate: floor, grace, debounce, latch
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "airflow.h"

airflow_state_t airflow_tick(airflow_fan_t *f, double reading, double floor, int in_grace)
{
    if (floor <= 0.0) {
        f->under_ticks = 0;
        f->tripped = 0;
        f->state = Air_Off;
        return f->state;
    }
    if (f->tripped) {
        f->state = Air_Tripped;
        return f->state;
    }
    if (in_grace) {
        f->under_ticks = 0;
        f->state = Air_Grace;
        return f->state;
    }
    if (reading >= floor) {
        f->under_ticks = 0;
        f->state = Air_Ok;
        return f->state;
    }
    f->under_ticks++;
    if (f->under_ticks >= AIRFLOW_DEBOUNCE_TICKS) {
        f->tripped = 1;
        f->state = Air_Tripped;
    } else {
        f->state = Air_Under;
    }
    return f->state;
}

void airflow_reset(airflow_fan_t *f)
{
    f->under_ticks = 0;
    f->tripped = 0;
    f->state = Air_Off;
}

const char *airflow_state_name(airflow_state_t s)
{
    switch (s) {
    case Air_Grace:   return "grace";
    case Air_Ok:      return "ok";
    case Air_Under:   return "under";
    case Air_Tripped: return "TRIPPED";
    default:          return "off";
    }
}

double airflow_rpm(long period, double units_per_second, int pulses_per_rev)
{
    if (period <= 0 || pulses_per_rev <= 0)
        return 0.0;
    return units_per_second * 60.0 / ((double)period * pulses_per_rev);
}

long airflow_run_duty(long local, long job, int armed)
{
    if (job < 0)
        return local;
    if (armed && job < local)
        return local;
    return job;
}

int airflow_judged(int run, int armed, long duty, long local)
{
    if (!run)
        return 0;
    return armed || duty >= local;
}

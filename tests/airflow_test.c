/*
 * airflow_test.c - host unit test for the per-fan airflow gate
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The gate's rules, pinned: nothing counts in the grace window, three
 * consecutive ticks under the floor trip it and a good tick in between
 * clears the count, a trip latches until the next run session whatever
 * the fan does afterward, and a floor of zero is the gate off (no count,
 * no latch, and a standing trip cleared). Plus the tach conversions the
 * floors are measured in, and the operating-point rules: a job may not
 * lower a fan below run duty while armed, and a gate judges a fan only
 * armed or at run duty (a hunt with its fans off is not judged).
 */
#include "../src/airflow.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    airflow_fan_t f;
    airflow_reset(&f);

    printf("grace and ok\n");
    CHECK(airflow_tick(&f, 0, 3700, 1) == Air_Grace, "a stopped fan in grace is grace, not under");
    CHECK(airflow_tick(&f, 0, 3700, 1) == Air_Grace && f.under_ticks == 0, "grace never counts");
    CHECK(airflow_tick(&f, 6700, 3700, 0) == Air_Ok, "at speed after grace: ok");
    CHECK(airflow_tick(&f, 3700, 3700, 0) == Air_Ok, "exactly at the floor: ok");

    printf("debounce\n");
    CHECK(airflow_tick(&f, 1000, 3700, 0) == Air_Under && f.under_ticks == 1, "one tick under: under");
    CHECK(airflow_tick(&f, 1000, 3700, 0) == Air_Under && f.under_ticks == 2, "two ticks under: still under");
    CHECK(airflow_tick(&f, 6700, 3700, 0) == Air_Ok && f.under_ticks == 0, "a good tick clears the count");
    CHECK(airflow_tick(&f, 1000, 3700, 0) == Air_Under, "under again: one");
    CHECK(airflow_tick(&f, 1000, 3700, 0) == Air_Under, "two");
    CHECK(airflow_tick(&f, 1000, 3700, 0) == Air_Tripped && f.tripped, "three consecutive: TRIPPED");

    printf("latch\n");
    CHECK(airflow_tick(&f, 6700, 3700, 0) == Air_Tripped, "recovery does not clear a trip");
    CHECK(airflow_tick(&f, 6700, 3700, 1) == Air_Tripped, "grace does not clear a trip");
    airflow_reset(&f);
    CHECK(!f.tripped && f.under_ticks == 0 && f.state == Air_Off, "a new run session clears it");
    CHECK(airflow_tick(&f, 6700, 3700, 0) == Air_Ok, "and the gate works again");

    printf("off by value\n");
    CHECK(airflow_tick(&f, 0, 0, 0) == Air_Off, "a floor of zero: off");
    CHECK(airflow_tick(&f, 0, 0, 0) == Air_Off && f.under_ticks == 0, "off never counts");
    CHECK(airflow_tick(&f, 0, -1, 0) == Air_Off, "a negative floor: off");
    airflow_tick(&f, 0, 3700, 0);
    airflow_tick(&f, 0, 3700, 0);
    CHECK(airflow_tick(&f, 0, 3700, 0) == Air_Tripped, "trip it");
    CHECK(airflow_tick(&f, 0, 0, 0) == Air_Off && !f.tripped, "turning the gate off clears a standing trip");
    CHECK(airflow_tick(&f, 0, 3700, 0) == Air_Under && f.under_ticks == 1, "turned back on, it counts from zero");

    printf("names and conversions\n");
    CHECK(!strcmp(airflow_state_name(Air_Off), "off") && !strcmp(airflow_state_name(Air_Grace), "grace") &&
          !strcmp(airflow_state_name(Air_Ok), "ok") && !strcmp(airflow_state_name(Air_Under), "under") &&
          !strcmp(airflow_state_name(Air_Tripped), "TRIPPED"), "state names");
    CHECK(airflow_rpm(0, 1e9, 2) == 0.0, "a zero period is a stopped fan");
    CHECK(airflow_rpm(-5, 1e9, 2) == 0.0, "a negative period is a stopped fan");
    CHECK((long)airflow_rpm(4442000, 1e9, 2) == 6753, "exhaust: 4.442 ms at 2 pulses/rev is 6753 rpm");
    CHECK((long)airflow_rpm(41000000, 1e9, 2) == 731, "intake: 41 ms at 2 pulses/rev is 731 rpm");
    CHECK((long)airflow_rpm(3900, 1e6, 8) == 1923, "air assist: 3900 us at 8 pulses/rev is 1923 rpm");
    CHECK((long)airflow_rpm(64500, 1e6, 8) == 116, "the factory's AArx 64500 us is a 116 rpm floor");

    printf("run duty: a job may only raise a fan while armed\n");
    CHECK(airflow_run_duty(65535, -1, 0) == 65535 && airflow_run_duty(65535, -1, 1) == 65535, "no job duty: the configured run duty, armed or not");
    CHECK(airflow_run_duty(65535, 0, 0) == 0, "unarmed, a job's 0 (a hunt) stands");
    CHECK(airflow_run_duty(65535, 0, 1) == 65535, "armed, a job's 0 is raised to run duty");
    CHECK(airflow_run_duty(43278, 30000, 1) == 43278, "armed, a lower job duty is raised to run duty");
    CHECK(airflow_run_duty(43278, 60000, 1) == 60000 && airflow_run_duty(43278, 60000, 0) == 60000, "a higher job duty stands either way");

    printf("judged: armed, or at the run duty the floor was measured at\n");
    CHECK(!airflow_judged(0, 1, 65535, 65535), "not in run: never judged");
    CHECK(airflow_judged(1, 1, 0, 65535), "armed: judged whatever the duty says");
    CHECK(airflow_judged(1, 0, 65535, 65535), "unarmed at run duty (a bare M8): judged");
    CHECK(airflow_judged(1, 0, 65536, 65535), "unarmed above run duty: judged");
    CHECK(!airflow_judged(1, 0, 0, 65535), "unarmed with the fan off (a hunt's exhaust): not judged");
    CHECK(!airflow_judged(1, 0, 204, 1023), "unarmed at idle duty (a hunt's air assist): not judged");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

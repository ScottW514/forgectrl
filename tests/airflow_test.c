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
 * floors are measured in.
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

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

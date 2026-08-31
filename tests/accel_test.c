/*
 * accel_test.c - host unit test for the crash-watch tier logic
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The tiers, pinned: an IG1 event raises the pause tier once and a
 * quiet run of polls releases it; an IG2 event latches the fail tier
 * for the session whatever comes after; a session reset clears both;
 * the axis bits ride along for the log line.
 */
#include "../src/accel.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
    crash_watch_t w;
    crash_reset(&w);

    printf("quiet\n");
    CHECK(crash_tick(&w, 0, 0) == CrashEv_None && !w.alert && !w.alarm,
          "no sources: nothing");
    CHECK(crash_tick(&w, CRASH_SRC_XH, 0) == CrashEv_None,
          "axis bits without IA are noise, not an event");

    printf("alert tier\n");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_Alert,
          "IG1 IA raises the alert");
    CHECK(w.alert && w.axes == CRASH_SRC_XH, "and records the axis");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_YH, 0) == CrashEv_None,
          "a standing alert is raised once");
    CHECK(w.axes == CRASH_SRC_YH, "but the axis record follows the signal");
    for (int i = 0; i < CRASH_CLEAR_TICKS - 1; i++)
        CHECK(crash_tick(&w, 0, 0) == CrashEv_None && w.alert,
              "a quiet poll under the count keeps holding");
    CHECK(crash_tick(&w, 0, 0) == CrashEv_Released && !w.alert,
          "the quiet count releases the alert");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_Alert,
          "a new event raises it again");
    crash_tick(&w, 0, 0);
    crash_tick(&w, 0, 0);
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_None
          && w.clear_ticks == 0,
          "an event inside the quiet count restarts it");

    printf("fail tier\n");
    crash_reset(&w);
    CHECK(crash_tick(&w, 0, CRASH_SRC_IA | CRASH_SRC_XH | CRASH_SRC_YH)
          == CrashEv_Alarm, "IG2 IA latches the alarm");
    CHECK(w.alarm && w.axes == (CRASH_SRC_XH | CRASH_SRC_YH),
          "with both axes recorded");
    CHECK(crash_tick(&w, 0, 0) == CrashEv_None && w.alarm,
          "quiet polls do not clear it");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_None
          && w.alarm, "an alert event after the alarm changes nothing");
    CHECK(crash_tick(&w, 0, CRASH_SRC_IA) == CrashEv_None,
          "a second abort event is not a second alarm");

    printf("alert then abort\n");
    crash_reset(&w);
    crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0);
    CHECK(crash_tick(&w, 0, CRASH_SRC_IA | CRASH_SRC_YH) == CrashEv_Alarm,
          "an abort during a standing alert still latches the alarm");
    CHECK(w.alarm, "latched");

    printf("session reset\n");
    crash_reset(&w);
    CHECK(!w.alert && !w.alarm && w.clear_ticks == 0 && w.axes == 0,
          "a new run session clears both tiers");
    CHECK(crash_tick(&w, CRASH_SRC_IA | CRASH_SRC_XH, 0) == CrashEv_Alert,
          "and the watch works again");

    printf("axis names\n");
    CHECK(crash_axes_name(CRASH_SRC_XH)[0] == 'x', "x alone");
    CHECK(crash_axes_name(CRASH_SRC_YH)[0] == 'y', "y alone");
    CHECK(crash_axes_name(CRASH_SRC_XH | CRASH_SRC_YH)[1] == 'y', "both");
    CHECK(crash_axes_name(0)[0] == '-', "none");

    printf("%s\n", failures ? "FAILURES" : "all passed");
    return failures ? 1 : 0;
}

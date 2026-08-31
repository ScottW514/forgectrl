/*
 * curverec_fit_test.c - host unit test for the dose-curve fitter
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Drives curverec_fit() with synthetic 25 Hz traces shaped like the
 * bench ladders: seven rungs of discharge with thermopile deltas rising
 * over a drifting baseline, dark gaps between. The fit must segment on
 * the gaps, read each rung's delta over its local baseline, normalize
 * to the full rung, and refuse a trace with a missing rung or a
 * non-rising ladder.
 */
#define _GNU_SOURCE
#include "../src/curverec.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

#define HZ 25
#define N  (HZ * 60 * 6)
static long hv[N], tp[N];
static int n;

static void reset(void) { n = 0; memset(hv, 0, sizeof(hv)); memset(tp, 0, sizeof(tp)); }

static void dark(double secs, long base)
{
    for (int i = 0; i < (int)(secs * HZ) && n < N; i++, n++) {
        hv[n] = 3;
        tp[n] = base;
    }
}

static void rung(double secs, long cur, long base, long delta)
{
    for (int i = 0; i < (int)(secs * HZ) && n < N; i++, n++) {
        hv[n] = cur;
        tp[n] = base + delta;
    }
}

static const long DELTAS[] = { 8, 33, 115, 345, 608, 822, 1643 };

static void ladder(int rungs, long base)
{
    dark(5.0, base);
    for (int i = 0; i < rungs; i++) {
        rung(10.0, 400 + 80 * i, base + 2 * i, DELTAS[i]);   /* baseline drifts */
        dark(4.0, base + 2 * i);
    }
    dark(25.0, base + 14);
}

int main(void)
{
    curverec_pt pts[8];
    char err[96];

    printf("the dose-curve fitter:\n");

    reset();
    ladder(7, 1830);
    int got = curverec_fit(hv, tp, n, HZ, pts, 8, err, sizeof(err));
    CHECK(got == 7, "a clean seven-rung ladder fits seven points");
    if (got == 7) {
        CHECK(pts[0].density == 10 && pts[6].density == 100,
              "the densities are the ladder's rungs");
        CHECK(fabs(pts[6].light - 100.0) < 0.01, "the full rung normalizes to 100");
        CHECK(fabs(pts[5].light - 100.0 * 822 / 1643) < 1.0,
              "the 80 percent rung reads its measured half of full");
        int mono = 1;
        for (int i = 1; i < 7; i++)
            mono &= pts[i].light > pts[i - 1].light;
        CHECK(mono, "the fitted lights are strictly increasing");
    }

    reset();
    ladder(6, 1830);
    got = curverec_fit(hv, tp, n, HZ, pts, 8, err, sizeof(err));
    CHECK(got == -1 && strstr(err, "6 discharge segments"),
          "a missing rung is refused with the count");

    reset();
    dark(5.0, 1830);
    rung(10.0, 500, 1830, 300);
    dark(1.5, 1830);                    /* a blip inside the merge window */
    rung(10.0, 500, 1830, 300);
    dark(25.0, 1830);
    got = curverec_fit(hv, tp, n, HZ, pts, 8, err, sizeof(err));
    CHECK(got == -1, "short gaps merge: this is one rung, not a ladder");

    reset();
    dark(5.0, 1830);
    for (int i = 0; i < 7; i++) {
        rung(10.0, 500, 1830, i == 3 ? 100 : DELTAS[i]);
        dark(4.0, 1830);
    }
    dark(25.0, 1830);
    got = curverec_fit(hv, tp, n, HZ, pts, 8, err, sizeof(err));
    CHECK(got == -1 && strstr(err, "does not rise"),
          "a non-rising rung is refused as not a clean ladder");

    reset();
    dark(30.0, 1830);
    got = curverec_fit(hv, tp, n, HZ, pts, 8, err, sizeof(err));
    CHECK(got == -1, "an all-dark trace is refused");

    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the fitter segments the ladder, normalizes to full, "
                      "and refuses what is not a clean ladder\n",
           failures);
    return failures ? 1 : 0;
}

/*
 * camhealth_test.c - host unit test for the capture frame-health ladder
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A frame the capture queue flags errored must never reach a demosaic - it
 * would be served to an operator as a picture of the bed - so the classifier
 * is checked here rather than only on hardware that has to be misbehaving to
 * exercise it. The properties that matter: an errored frame is never USE; a
 * run of them escalates to a stream restart and then to giving up; warm-up
 * frames after a restart do not count as recovery (otherwise a sensor that
 * emits two clean frames per restart would loop forever); and one usable
 * frame puts the ladder back at the bottom.
 */
#define _GNU_SOURCE
#include "../src/camhealth.h"

#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* Feed n frames of one kind, returning the last verdict. */
static cam_frame_action_t feed(struct cam_health *h, int errored, int n)
{
    cam_frame_action_t a = CAM_FRAME_USE;

    for (int i = 0; i < n; i++)
        a = cam_health_frame(h, errored);
    return a;
}

static void test_warmup(void)
{
    struct cam_health h;
    int all_warmup = 1;

    cam_health_started(&h);
    for (int i = 0; i < CAM_WARMUP_FRAMES; i++)
        if (cam_health_frame(&h, 0) != CAM_FRAME_WARMUP)
            all_warmup = 0;
    CHECK(all_warmup, "the first frames after a start are dropped");
    CHECK(cam_health_frame(&h, 0) == CAM_FRAME_USE,
          "the frame after the warm-up is used");
    CHECK(cam_health_frame(&h, 0) == CAM_FRAME_USE,
          "and so is every good frame after that");
}

static void test_errored_never_used(void)
{
    struct cam_health h;
    int never_used = 1;

    cam_health_started(&h);
    /* Well past every threshold, through restarts and the abort. */
    for (int i = 0; i < 200; i++) {
        cam_frame_action_t a = cam_health_frame(&h, 1);
        if (a == CAM_FRAME_USE || a == CAM_FRAME_WARMUP)
            never_used = 0;
        if (a == CAM_FRAME_RESTART)
            cam_health_restarted(&h);
    }
    CHECK(never_used, "an errored frame is never USE and never a warm-up");
}

static void test_isolated_errors(void)
{
    struct cam_health h;
    int only_drops = 1;

    cam_health_started(&h);
    feed(&h, 0, CAM_WARMUP_FRAMES + 1);
    /* One short of the threshold, cleared by a good frame, repeatedly: a
     * stream that drops the odd frame must not be restarted. */
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < CAM_MAX_BAD_FRAMES - 1; i++)
            if (cam_health_frame(&h, 1) != CAM_FRAME_DROP)
                only_drops = 0;
        if (cam_health_frame(&h, 0) != CAM_FRAME_USE)
            only_drops = 0;
    }
    CHECK(only_drops, "occasional errored frames are dropped, not escalated");
}

static void test_restart_threshold(void)
{
    struct cam_health h;

    cam_health_started(&h);
    feed(&h, 0, CAM_WARMUP_FRAMES + 1);
    CHECK(feed(&h, 1, CAM_MAX_BAD_FRAMES - 1) == CAM_FRAME_DROP,
          "errored frames below the threshold only drop");
    CHECK(cam_health_frame(&h, 1) == CAM_FRAME_RESTART,
          "the threshold errored frame restarts the stream");
    /* The bad-frame count restarts too, so the next escalation needs a
     * fresh run rather than one more frame. */
    cam_health_restarted(&h);
    feed(&h, 0, CAM_WARMUP_FRAMES);
    CHECK(feed(&h, 1, CAM_MAX_BAD_FRAMES - 1) == CAM_FRAME_DROP,
          "the run counter resets after a restart");
}

static void test_escalates_to_abort(void)
{
    struct cam_health h;
    int restarts = 0;
    cam_frame_action_t a = CAM_FRAME_USE;

    cam_health_started(&h);
    /* A sensor that emits its warm-up frames cleanly and then nothing but
     * errors: the restart must stop being retried. */
    for (int i = 0; i < 500 && a != CAM_FRAME_ABORT; i++) {
        a = cam_health_frame(&h, h.warmup > 0 ? 0 : 1);
        if (a == CAM_FRAME_RESTART) {
            restarts++;
            cam_health_restarted(&h);
        }
    }
    CHECK(a == CAM_FRAME_ABORT, "endless corruption ends in ABORT");
    CHECK(restarts == CAM_MAX_RECOVERIES,
          "it aborts after exactly CAM_MAX_RECOVERIES restarts");
}

static void test_good_frame_clears_restarts(void)
{
    const int rounds = 20;
    struct cam_health h;
    int restarts = 0, aborted = 0, recovered = 1;

    cam_health_started(&h);
    feed(&h, 0, CAM_WARMUP_FRAMES + 1);
    /* A stream that goes bad, is restarted, and then does deliver a usable
     * frame: that is a genuine recovery, so the ladder must go back to the
     * bottom every time however often it happens. */
    for (int round = 0; round < rounds; round++) {
        cam_frame_action_t a = feed(&h, 1, CAM_MAX_BAD_FRAMES);
        if (a == CAM_FRAME_ABORT) {
            aborted = 1;
            break;
        }
        restarts++;
        cam_health_restarted(&h);
        feed(&h, 0, CAM_WARMUP_FRAMES);
        if (cam_health_frame(&h, 0) != CAM_FRAME_USE)
            recovered = 0;
    }
    CHECK(!aborted, "a usable frame between restarts prevents the abort");
    CHECK(restarts == rounds && recovered,
          "and the stream keeps being recovered");
}

int main(void)
{
    printf("camhealth_test\n");
    test_warmup();
    test_errored_never_used();
    test_isolated_errors();
    test_restart_threshold();
    test_escalates_to_abort();
    test_good_frame_clears_restarts();
    printf(failures ? "FAILED (%d)\n" : "PASSED\n", failures);
    return failures ? 1 : 0;
}

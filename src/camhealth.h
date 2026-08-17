/*
 * camhealth.h - what to do with a frame the capture queue just handed back
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The capture queue flags a buffer V4L2_BUF_FLAG_ERROR when the frame in it
 * is short, torn or arrived after the receiver lost CSI-2 sync. Such a frame
 * must never reach a demosaic: a snapshot built from one is a corrupt image
 * presented to an operator as a picture of the bed, and a stream built from
 * one is worse because it looks live.
 *
 * The policy is a ladder, kept here rather than inline in the capture loop so
 * it can be exercised without a sensor: drop an errored frame; if they keep
 * coming, cycle the queue, which is what re-synchronizes the receiver; if
 * cycling stops helping, give up and let the engine tear down (clients
 * reconnect, and a reconnect re-runs the whole pipeline setup).
 *
 * The first frames after a stream starts are dropped for a different reason:
 * they were already in flight while the sensor was being programmed, so they
 * predate its exposure settling.
 *
 * Not thread-safe: one struct per capture, touched by the capture thread.
 */
#ifndef FORGECTRL_CAMHEALTH_H
#define FORGECTRL_CAMHEALTH_H

/* Frames discarded after STREAMON (see above). */
#define CAM_WARMUP_FRAMES   2
/* Consecutive errored frames that trigger a stream restart. */
#define CAM_MAX_BAD_FRAMES  4
/* Restarts with no usable frame between them before the engine gives up. */
#define CAM_MAX_RECOVERIES  3

typedef enum {
    CAM_FRAME_USE,      /* good, and past the warm-up: demosaic it */
    CAM_FRAME_WARMUP,   /* good but early: requeue it unused */
    CAM_FRAME_DROP,     /* errored: requeue it unused */
    CAM_FRAME_RESTART,  /* errored, and they are piling up: cycle the queue */
    CAM_FRAME_ABORT,    /* errored, and cycling has stopped helping */
} cam_frame_action_t;

struct cam_health {
    int warmup;      /* good frames still to drop after a (re)start */
    int bad;         /* consecutive errored frames */
    int restarts;    /* stream restarts since the last usable frame */
};

/* A capture just started (STREAMON on a freshly configured pipeline). */
void cam_health_started(struct cam_health *h);

/* The stream was just cycled by CAM_FRAME_RESTART and came back up. */
void cam_health_restarted(struct cam_health *h);

/* Classify a dequeued frame; `errored` is the V4L2_BUF_FLAG_ERROR test.
 * Advances the ladder, so call exactly once per dequeued buffer. */
cam_frame_action_t cam_health_frame(struct cam_health *h, int errored);

#endif

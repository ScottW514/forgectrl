/*
 * camhealth.c - capture-queue frame health ladder (contract in camhealth.h)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "camhealth.h"

void cam_health_started(struct cam_health *h)
{
    h->warmup = CAM_WARMUP_FRAMES;
    h->bad = 0;
    h->restarts = 0;
}

void cam_health_restarted(struct cam_health *h)
{
    h->warmup = CAM_WARMUP_FRAMES;
    h->bad = 0;
    h->restarts++;
}

cam_frame_action_t cam_health_frame(struct cam_health *h, int errored)
{
    if (errored) {
        if (++h->bad < CAM_MAX_BAD_FRAMES)
            return CAM_FRAME_DROP;
        h->bad = 0;
        return h->restarts >= CAM_MAX_RECOVERIES ? CAM_FRAME_ABORT
                                                 : CAM_FRAME_RESTART;
    }

    h->bad = 0;
    /* A warm-up frame is not proof the stream recovered - after a restart the
     * first frames out are always good-looking - so the restart counter only
     * clears on a frame that was actually usable. Otherwise a sensor that
     * produces two clean frames and then falls over again would restart
     * forever instead of escalating. */
    if (h->warmup > 0) {
        h->warmup--;
        return CAM_FRAME_WARMUP;
    }
    h->restarts = 0;
    return CAM_FRAME_USE;
}

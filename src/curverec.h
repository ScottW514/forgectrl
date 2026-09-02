/*
 * curverec.h - the owner-run dose-curve recorder (see curverec.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_CURVEREC_H
#define FORGECTRL_CURVEREC_H

#include <stddef.h>

/* Start a recording: refuses while a sender is connected (the recorder
 * is the sender for this job), saves and clears the laser floor and
 * curve keys so the ladder measures the raw dose response, streams the
 * ladder over the local Grbl socket (absolute from X0 Y0; the operator's
 * button press starts the fire, every gate standing), and samples the
 * tube current and the head thermopile until the ladder has played and
 * gone dark. 0 on start, -1 with the reason otherwise. */
int curverec_start(char *err, size_t elen);

/* Stop and fit now (the auto-stop needs 20 s of dark after the last
 * rung); also the abort path - the saved keys are restored either way. */
void curverec_stop(void);
void curverec_init(void);

/* The recorder state as JSON: state (idle|waiting|recording|done|
 * failed), reason, elapsed_s, samples, and on done the fitted points
 * and the curve string ready for laser_dose_curve. */
int curverec_status_json(char *buf, size_t len);

/* The ladder G-code the operator runs from the sender, matched to the
 * fitter's expectations (one 100 mm line per rung, laser off between). */
int curverec_ladder_gcode(char *buf, size_t len);

/* The pure fitter, exported for the host test: hv and tp are parallel
 * samples at rate hz; on success writes up to max_pts density:light
 * points (light in percent of the last rung) and returns the count, on
 * failure returns -1 with the reason in err. */
typedef struct {
    double density;
    double light;
} curverec_pt;

int curverec_fit(const long *hv, const long *tp, int n, double hz,
                 curverec_pt *pts, int max_pts, char *err, size_t elen);

#endif

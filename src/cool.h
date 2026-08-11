/*
 * cool.h - cooling engine (see cool.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_COOL_H
#define FORGECTRL_COOL_H

#include <stddef.h>

/* Start the engine: resolve tunables (GFCOOL_* env > cool_* settings >
 * compiled defaults), take the idle posture (pump on, TEC off, purge
 * on, heater off, fans idle), and launch the 1 Hz policy thread that
 * writes /run/forgefirm/cooling.state. */
void cool_init(void);

/* Stop the thread, stand the hardware down to the idle posture and
 * remove the verdict file (readers treat a missing file as
 * fire-blocked). */
void cool_shutdown(void);

/* Job-state report from the active controller (POST /cool/state).
 * mode is "idle", "run" or "cooldown"; armed = laser armed (forces the
 * run profile and flow interrogation whatever mode says). The duty
 * arguments override the run fan profile for this job; pass -1 to use
 * the configured/factory values. Returns 0, or -1 on a bad mode. */
int cool_state_report(const char *mode, int armed,
                      long air_assist, long exhaust, long intake);

/* Engine state as JSON (verdict, temps, phase, last report age) for
 * the UI and bench tooling. */
int cool_status_json(char *buf, size_t len);

#endif

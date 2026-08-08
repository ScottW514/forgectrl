/*
 * diag.h - forgectrl: hardware diagnostics runner
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef DIAG_H
#define DIAG_H

#include <stddef.h>

/* Startup recovery: if a diagnostic died mid-run (stale marker file),
 * stand the hardware down and restart the motion controller. */
void diag_init(void);

/* Launch a tool ("flow-verify" or "flow-calibrate") on the runner
 * thread. Returns 0 on start, -1 if a diagnostic is already running,
 * -2 if the machine is not idle, -3 for an unknown tool. */
int diag_start(const char *tool);

/* Request an abort; the runner stands down and restarts the controller. */
void diag_abort(void);

/* Live runner state as JSON (phase, temperatures, log, result). */
int diag_status_json(char *buf, size_t len);

/* True while a diagnostic owns the hardware (locks settings writes). */
int diag_running(void);

#endif

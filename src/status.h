/*
 * status.h - machine operational status (see status.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_STATUS_H
#define FORGECTRL_STATUS_H

#include <stddef.h>

/* Build the /status JSON (motion state, position when homed, fans,
 * coolant, switches) into buf. Always succeeds; unreadable sources are
 * omitted or defaulted. Needs ~512 bytes. */
int machine_status_json(char *buf, size_t len);

/* True only when the motion driver reports the idle state. Fails closed:
 * any read failure - including fd exhaustion under a connection flood
 * (EMFILE) - reports not-idle, so a destructive action (flash, mode
 * switch, diag) is never permitted on a bad read. Settings changes and
 * takeovers are only allowed while idle. */
int machine_is_idle(void);

/* Factory coolant-thermistor conversion (shared with the diagnostics
 * runner). Raw 10-bit ADC count -> degrees C; out-of-range input maps
 * to -273.15. */
double coolant_degc(long raw);

#endif

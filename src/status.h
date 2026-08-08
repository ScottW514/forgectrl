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

/* True when the motion driver reports the idle state (or there is no
 * driver). Settings changes are only allowed while idle. */
int machine_is_idle(void);

#endif

/*
 * liveness.h - motion-liveness probe (see liveness.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LIVENESS_H
#define FORGECTRL_LIVENESS_H

#include <stddef.h>

/* Command a small motion through the pulse-device fd and verify it
 * PHYSICALLY happened via the head accelerometer. Requires the kernel
 * idle and no motion controller running (the supervisor guarantees
 * both). Returns 1 = motion confirmed, 0 = the gantry did not move
 * (wedged stepper drivers), -1 = probe could not run. detail gets a
 * short human-readable result line either way. */
int liveness_probe(int pulse_fd, char *detail, size_t dlen);

#endif

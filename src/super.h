/*
 * super.h - controller-mode supervisor (see super.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_SUPER_H
#define FORGECTRL_SUPER_H

#include <stddef.h>

/* Start the supervisor thread and spawn the controller selected by the
 * controller_mode setting (grbl unless set to cloud). If an unmanaged
 * controller process is already running (legacy init scripts), the
 * supervisor stands by instead of fighting it. */
void super_init(void);

/* Stop the supervised controller (SIGTERM, escalating) and the thread. */
void super_shutdown(void);

/* Switch the active controller mode ("grbl" or "cloud"): idle-gated,
 * stops the active controller, persists the setting, starts the other,
 * and waits for its first job-state report. Returns 0 on success, -1
 * with err filled on refusal/failure. */
int super_mode_switch(const char *mode, char *err, size_t elen);

/* Diagnostics takeover: stop the active controller without changing the
 * selected mode (returns 0 once it is down), and start it again. The
 * controller that comes back is the selected mode's - whichever that is. */
int super_controller_stop(void);
void super_controller_start(void);

/* {"mode":"grbl","controller":"running|stopped|standby","pid":N} */
int super_status_json(char *buf, size_t len);

#endif

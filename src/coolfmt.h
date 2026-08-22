/*
 * coolfmt.h - the /cool/status document: pure formatters, sized fragments
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The engine publishes its state as JSON fragments it renders once per
 * tick and stitches into one document on request. Every fragment has a
 * fixed-size buffer; a value set that does not fit is a defect, never a
 * cut-off fragment: each formatter returns -1 and leaves a valid empty
 * fragment behind, so the document a client parses is always JSON. The
 * sizes here are the ones the engine uses, and tests/coolfmt_test.c
 * renders the widest legal values into them.
 */
#ifndef COOLFMT_H
#define COOLFMT_H

#include <stddef.h>

#include "cool.h"

/* Fragment buffer sizes (bytes, NUL included). */
#define COOL_VERDICT_MAX        12
#define COOL_REASON_MAX         112
#define COOL_GATES_OFF_JSON_MAX 128
#define COOL_LIMITS_JSON_MAX    256
#define COOL_FAN_GATES_JSON_MAX 512

/* The scalar fields and the document's own punctuation, at their widest
 * rendering (coolfmt_test measures it; the build fails if it no longer
 * fits under the fragments). */
#define COOL_STATUS_JSON_SCALARS 256

_Static_assert(COOL_REASON_MAX + COOL_GATES_OFF_JSON_MAX + COOL_LIMITS_JSON_MAX +
               COOL_FAN_GATES_JSON_MAX + COOL_STATUS_JSON_SCALARS <= COOL_STATUS_JSON_MAX,
               "the /cool/status fragments outgrew COOL_STATUS_JSON_MAX");

/* The effective limits object. from_header names the source of the
 * coolant ceiling; critical_c is the local critical line (no header
 * carries one). Returns 0, or -1 with buf = "{}" when it did not fit. */
int coolfmt_limits(char *buf, size_t len, double max_c, double resume_c,
                   double critical_c, int from_header, const cool_limits_t *eff);

/* One fan's gate row. */
typedef struct {
    const char *name;       /* JSON key: exhaust, intake_1, ... */
    double reading;         /* rpm, or purge current in counts */
    double floor;           /* the effective floor, 0 = gate off */
    const char *state;      /* airflow state name, "TRIPPED", "off", "idle" */
} coolfmt_fan_t;

/* The per-fan gate object. Returns 0, or -1 with buf = "{}" when the
 * rows did not fit. */
int coolfmt_fan_gates(char *buf, size_t len, const coolfmt_fan_t *fans, size_t n);

/* The whole document. The three fragment strings are JSON already. */
typedef struct {
    const char *phase, *verdict, *reason, *fire_watch;
    int fire_ok, hold, armed;
    double down_c, up_c, report_age_s;
    const char *gates_off, *limits, *fan_gates;
} coolfmt_status_t;

/* Returns 0, or -1 when len was too small for the document. */
int coolfmt_status(char *buf, size_t len, const coolfmt_status_t *s);

#endif

/*
 * sanitize.h - forgectrl: log-export redaction (see sanitize.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A line filter for the log export: known machine-specific values
 * (serial, hostname, credentials, WiFi SSID/PSK) are replaced by named
 * placeholders, and pattern classes (network addresses, e-mail
 * addresses, bearer tokens, long hex/base64 blobs) by numbered
 * placeholders that stay stable within one export, so correlation
 * across files survives. Pure C, no system access: the caller feeds
 * the known values; host unit tests feed fixtures.
 */
#ifndef FORGECTRL_SANITIZE_H
#define FORGECTRL_SANITIZE_H

#include <stddef.h>
#include <stdio.h>

typedef struct sanitizer sanitizer_t;

/* Create / destroy a sanitizer. All state (placeholder numbering,
 * statistics) lives in the object; use one per export. */
sanitizer_t *san_new(void);
void san_free(sanitizer_t *s);

/* Register a known value; every boundary-delimited occurrence becomes
 * "<TAG>". Values shorter than 3 characters (5 for all-digit values)
 * are ignored - too likely to hit unrelated text. Longer values are
 * matched first. */
void san_add_known(sanitizer_t *s, const char *tag, const char *value);

/* Sanitize one line (with or without its newline). Returns a malloc'd
 * string the caller frees, or NULL on allocation failure. */
char *san_line(sanitizer_t *s, const char *line);

/* Total redactions so far, and a per-class report ("<TAG>: n" lines). */
unsigned long san_total(const sanitizer_t *s);
void san_report(const sanitizer_t *s, FILE *out);

#endif /* FORGECTRL_SANITIZE_H */

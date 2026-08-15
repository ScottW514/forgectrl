/*
 * logs.h - forgectrl: the ForgeFIRM logging tree (see logs.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_LOGS_H
#define FORGECTRL_LOGS_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

/* Where every ForgeFIRM logger's files live: LOGS_ROOT/<logger>/. */
#define LOGS_ROOT      "/data/log/forgefirm"
/* Rendered rsyslog rules (read by rsyslog at boot). */
#define LOGS_RSYSLOG   "/data/forgefirm/rsyslog-forgefirm.conf"
/* Levels in force since boot (tmpfs; rewritten by every render). */
#define LOGS_EFFECTIVE "/var/run/forgefirm-loglevels"

/* The loggers, in display order. */
extern const char *const logs_names[];
extern const size_t logs_count;

/* Settings validators (log_<name>_disk / _remote, syslog_server,
 * syslog_port, syslog_proto). */
int logs_valid_level(const char *v);
int logs_valid_server(const char *v);
int logs_valid_port(const char *v);
int logs_valid_proto(const char *v);

/* Render LOGS_RSYSLOG and LOGS_EFFECTIVE from the settings, and create
 * the log directories. Run at boot before rsyslog starts (forgectrl
 * --render-syslog) so a settings change applies at the next reboot.
 * Returns 0, or -1 with the reason in err. */
int logs_render(char *err, size_t errlen);

/* JSON for GET /logs: the loggers with configured and effective levels
 * and on-disk sizes, the remote target, and whether a reboot is pending
 * to apply a change. Returns a malloc'd string. */
char *logs_list_json(void);

/* JSON for GET /logs/tail: the last `lines` lines of a logger's live
 * file, or - when `from` is a byte offset at or below the file's size -
 * everything appended since it (incremental follow). Returns a malloc'd
 * string, or NULL for an unknown logger. */
char *logs_tail_json(const char *name, long lines, long long from);

/* Export: a tar.gz bundle of the whole tree plus a system snapshot,
 * optionally sanitized. begin() stages the bundle (blocking; seconds)
 * and starts the archiver; read() streams the archive; end() reaps
 * and removes the staging. Only one export runs at a time: begin()
 * fails with err="busy" otherwise. settings_cb writes the settings
 * snapshot (secrets already masked) into the given stream. */
typedef struct logs_export logs_export_t;
logs_export_t *logs_export_begin(int sanitize, void (*settings_cb)(FILE *),
                                 char *err, size_t errlen);
ssize_t logs_export_read(logs_export_t *e, char *buf, size_t max);
void logs_export_end(logs_export_t *e);

/* Identity hooks the sanitizer needs, provided by main.c. */
unsigned long fuse_serial(void);
void machine_id(char *buf, size_t len);
int fuse_password(char *buf, size_t len);

#endif /* FORGECTRL_LOGS_H */

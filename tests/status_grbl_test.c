/*
 * status_grbl_test.c - host unit test for the /status grbl block
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The controller publishes its state to <run dir>/grbl.state; /status
 * echoes it as "grbl":{"age_s":...,"report":{...}} only while the
 * supervisor holds a live GRBL controller. A dead controller, an absent
 * file, a torn body (no closing brace) or an absurd age must all read
 * as no block at all - the panel treats absence as "no controller",
 * never as stale truth. Drives the real machine_status_json() with
 * GF_RUN_DIR pointed at a temp dir and the supervisor stubbed.
 */
#define _GNU_SOURCE
#include "../src/status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int diag_running(void) { return 0; }
static int grbl_up = 1;
int super_grbl_running(void) { return grbl_up; }

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static char dir[64];

static void put_state(const char *fmt, double ts)
{
    char path[128], body[512];
    snprintf(path, sizeof(path), "%s/grbl.state", dir);
    snprintf(body, sizeof(body), fmt, ts);
    FILE *f = fopen(path, "w");
    fputs(body, f);
    fclose(f);
}

static double now_mono(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    snprintf(dir, sizeof(dir), "/tmp/grblstat-XXXXXX");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("GF_RUN_DIR", dir, 1);
    setenv("GF_SYSFS_ROOT", dir, 1);    /* every machine attribute absent */

    char doc[3072];

    printf("/status grbl block:\n");

    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "no file: no grbl block");

    put_state("{\"ts_mono\":%.3f,\"state\":\"Idle\","
              "\"sender\":{\"connected\":true,\"generation\":3,\"for_s\":12,\"peer\":\"10.0.0.9\"},"
              "\"laser\":{\"armed\":false,\"arming\":false,\"model\":\"density\",\"floor_pct\":10},"
              "\"modals\":\"[GC:G0 G54]\",\"driver\":\"t\"}\n", now_mono());
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    if (!strstr(doc, "\"grbl\""))
        fprintf(stderr, "DOC: %s\n", doc);
    CHECK(strstr(doc, "\"grbl\":{\"age_s\":") != NULL, "fresh file: block present with age");
    CHECK(strstr(doc, "\"peer\":\"10.0.0.9\"") != NULL, "the report is embedded verbatim");
    CHECK(strstr(doc, "\"model\":\"density\"") != NULL, "the laser model rides the report");
    const char *p = strstr(doc, "\"age_s\":");
    CHECK(p && strtod(p + 8, NULL) < 5.0, "the age is computed from ts_mono");
    CHECK(doc[strlen(doc) - 1] == '}', "the document stays structurally intact");

    grbl_up = 0;
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "controller not running: no block, file or not");
    grbl_up = 1;

    put_state("{\"ts_mono\":%.3f,\"state\":\"Idle\"", now_mono());   /* torn: no brace */
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "a torn body reads as absent");

    put_state("{\"ts_mono\":%.3f,\"state\":\"Idle\",\"modals\":\"[GC:G0]\"}\n",
              now_mono() - 7200.0);
    machine_status_json(doc, sizeof(doc), "\"x\":1");
    CHECK(strstr(doc, "\"grbl\"") == NULL, "an absurd age reads as absent");

    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: the grbl block is echoed fresh and gated on a live controller\n",
           failures);
    return failures ? 1 : 0;
}

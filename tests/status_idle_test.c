/*
 * status_idle_test.c - host unit test for machine_is_idle() fail-closed
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Regression for X-2: machine_is_idle() must fail CLOSED - report
 * not-idle on any read failure, including the fd-table exhaustion an
 * unauthenticated connection flood can inflict (EMFILE). A read that
 * fails open would let a destructive action (flash, mode switch, diag)
 * or a mid-cut safing drop proceed on a machine that is actually busy.
 *
 * The test drives the real machine_is_idle() from status.c, pointing
 * its sysfs reader at a temp tree via GF_SYSFS_ROOT.
 */
#define _GNU_SOURCE
#include "../src/status.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

/* status.c's only external dependency. */
int diag_running(void) { return 0; }
int super_grbl_running(void) { return 0; }

static int failures;
static char root[256];       /* GF_SYSFS_ROOT, with trailing '/' */

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static void set_state(const char *val)
{
    char path[320];
    snprintf(path, sizeof(path), "%scnc", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%scnc/state", root);
    if (val) {
        FILE *f = fopen(path, "w");
        if (!f) { perror("fopen state"); exit(2); }
        fputs(val, f);
        fclose(f);
    } else {
        unlink(path);
    }
}

int main(void)
{
    char tmpl[] = "/tmp/forgectrl-status-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 2; }
    snprintf(root, sizeof(root), "%s/", dir);
    setenv("GF_SYSFS_ROOT", root, 1);

    printf("machine_is_idle() fail-closed (X-2):\n");

    set_state("idle");
    CHECK(machine_is_idle() == 1, "reads idle when cnc/state is \"idle\"");

    set_state("running");
    CHECK(machine_is_idle() == 0, "reads busy when cnc/state is \"running\"");

    set_state("idle\n");
    CHECK(machine_is_idle() == 1, "trailing newline still reads idle");

    set_state(NULL);
    CHECK(machine_is_idle() == 0, "fails closed when cnc/state is missing");

    /* EMFILE: the audit's actual trigger. With a valid "idle" state
     * present, exhaust the fd table so rd_attr's open() fails with
     * EMFILE - machine_is_idle must still report busy, not idle. */
    set_state("idle");
    CHECK(machine_is_idle() == 1, "reads idle again before fd exhaustion");
    struct rlimit rl, saved;
    getrlimit(RLIMIT_NOFILE, &saved);
    rl = saved;
    rl.rlim_cur = 64;                    /* small: quick to exhaust */
    setrlimit(RLIMIT_NOFILE, &rl);
    int held[64];
    int n = 0;
    while (n < 64) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) break;
        held[n++] = fd;
    }
    int idle_under_emfile = machine_is_idle();
    for (int i = 0; i < n; i++)
        close(held[i]);
    setrlimit(RLIMIT_NOFILE, &saved);
    CHECK(n > 0 && errno == EMFILE, "fd table was actually exhausted (EMFILE)");
    CHECK(idle_under_emfile == 0,
          "fails closed to busy under EMFILE with an \"idle\" state present");

    /* Cleanup. */
    set_state(NULL);
    char sub[320];
    snprintf(sub, sizeof(sub), "%scnc", root);
    rmdir(sub);
    rmdir(dir);

    printf(failures ? "FAIL: %d check(s) failed\n" : "PASS: machine_is_idle fails closed\n",
           failures);
    return failures ? 1 : 0;
}

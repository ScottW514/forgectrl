/*
 * lid_gate_test.c - host unit test for machine_lid_closed() fail-closed
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The camera privacy gate is only as good as its lid read. Every camera
 * entry point in cam.c refuses while machine_lid_closed() is false, so a
 * read that failed OPEN would let the sensors capture - and in cloud mode
 * upload - with the enclosure open, which is exactly the case the gate
 * exists to prevent.
 *
 * The positive direction needs the real switch hardware and is covered by
 * the bench acceptance test (camera.lid-privacy). What is provable on a
 * host, and what matters most, is that every way the read can fail lands
 * on "not closed": the device missing, the path not being an input device
 * at all, and the fd table exhausted under a connection flood.
 *
 * The test drives the real machine_lid_closed() from status.c through the
 * GF_SWITCH_DEV seam.
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
#include <sys/wait.h>
#include <unistd.h>

/* status.c's only external dependency. */
int diag_running(void) { return 0; }
int super_grbl_running(void) { return 0; }

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* machine_lid_closed() caches the device path on first use, so each case
 * runs in its own process. Returns the child's verdict. */
static int lid_closed_with(const char *dev)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        setenv("GF_SWITCH_DEV", dev, 1);
        _exit(machine_lid_closed() ? 1 : 0);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
        fprintf(stderr, "child did not exit normally\n");
        exit(2);
    }
    return WEXITSTATUS(st);
}

/* Same, with the fd table exhausted first so open() fails with EMFILE. */
static int lid_closed_under_emfile(const char *dev)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        setenv("GF_SWITCH_DEV", dev, 1);
        struct rlimit rl;
        getrlimit(RLIMIT_NOFILE, &rl);
        rl.rlim_cur = 64;
        setrlimit(RLIMIT_NOFILE, &rl);
        int n = 0;
        while (n < 64 && open("/dev/null", O_RDONLY) >= 0)
            n++;
        int closed = machine_lid_closed();
        /* 2 = the fd table was never actually exhausted (inconclusive) */
        _exit(errno != EMFILE ? 2 : (closed ? 1 : 0));
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st)) {
        fprintf(stderr, "child did not exit normally\n");
        exit(2);
    }
    return WEXITSTATUS(st);
}

int main(void)
{
    char tmpl[] = "/tmp/forgectrl-lid-XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 2; }

    char missing[320], regular[320];
    snprintf(missing, sizeof(missing), "%s/no-such-device", dir);
    snprintf(regular, sizeof(regular), "%s/not-an-input-device", dir);
    FILE *f = fopen(regular, "w");
    if (!f) { perror("fopen"); return 2; }
    fputs("this is not an evdev node", f);
    fclose(f);

    printf("machine_lid_closed() fail-closed (camera privacy gate):\n");

    CHECK(lid_closed_with(missing) == 0,
          "reports NOT closed when the switch device is missing");
    CHECK(lid_closed_with(regular) == 0,
          "reports NOT closed when the path is not an input device");
    CHECK(lid_closed_with("/dev/null") == 0,
          "reports NOT closed when EVIOCGSW is unsupported");

    int emfile = lid_closed_under_emfile(regular);
    CHECK(emfile != 2, "fd table was actually exhausted (EMFILE)");
    CHECK(emfile == 0, "reports NOT closed under EMFILE");

    unlink(regular);
    rmdir(dir);

    printf(failures ? "FAIL: %d check(s) failed\n"
                    : "PASS: machine_lid_closed fails closed\n", failures);
    return failures ? 1 : 0;
}

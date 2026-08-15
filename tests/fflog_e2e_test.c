/*
 * fflog_e2e_test.c - emitter half of the host end-to-end logging check
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Emits a fixed set of messages through the real fflog emitter under
 * ident "grblhal": one per severity, one with a trailing newline, one
 * long one. tests/fflog_e2e.sh drives it against a private rsyslogd
 * running the shipped rsyslog.conf plus rules rendered by
 * `forgectrl --render-syslog`, then checks what landed on disk.
 */
#define _GNU_SOURCE
#include "../src/fflog.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    fflog_init(argc > 1 ? argv[1] : "grblhal");
    fflog(LOG_ERR, "e2e error line");
    fflog(LOG_WARNING, "e2e warning line");
    fflog(LOG_NOTICE, "e2e notice line");
    fflog(LOG_INFO, "e2e info line with newline\n");
    fflog(LOG_DEBUG, "e2e debug line");
    char big[600];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    fflog(LOG_INFO, "e2e long %s", big);
    printf("emitted; dropped=%lu level=%d\n", fflog_dropped(), fflog_level());
    return 0;
}

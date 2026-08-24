/*
 * peer.c - is a socket peer this host?
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "peer.h"

#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

int peer_is_loopback(const struct sockaddr *sa)
{
    if (!sa)
        return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)sa;
        return (ntohl(s->sin_addr.s_addr) >> 24) == 127;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)sa;
        const uint8_t *a = s->sin6_addr.s6_addr;
        static const uint8_t lo[16] = { [15] = 1 };
        static const uint8_t mapped[12] = { [10] = 0xff, [11] = 0xff };
        if (!memcmp(a, lo, sizeof(lo)))
            return 1;                       /* ::1 */
        if (!memcmp(a, mapped, sizeof(mapped)) && a[12] == 127)
            return 1;                       /* ::ffff:127.0.0.0/8 */
    }
    return 0;
}

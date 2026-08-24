/*
 * auth_peer_test.c - host unit test for the loopback peer check
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The cooling report channel is open to this host alone. A dual-stack
 * listener reports an IPv4 client as a v4-mapped IPv6 peer, so every
 * spelling of "this host" must pass and everything else must not: a LAN
 * address in either family, a mapped LAN address, a link-local, the
 * unspecified address, another family, NULL, and a sockaddr_in6 whose
 * address bytes were lost to a sizeof(struct sockaddr) copy (what an
 * unpatched ulfius hands over), which must fail closed.
 */
#define _GNU_SOURCE
#include "../src/peer.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/un.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static int v4(const char *ip)
{
    struct sockaddr_in s = { .sin_family = AF_INET };
    inet_pton(AF_INET, ip, &s.sin_addr);
    return peer_is_loopback((const struct sockaddr *)&s);
}

static int v6(const char *ip)
{
    struct sockaddr_in6 s = { .sin6_family = AF_INET6 };
    inet_pton(AF_INET6, ip, &s.sin6_addr);
    return peer_is_loopback((const struct sockaddr *)&s);
}

int main(void)
{
    printf("this host:\n");
    CHECK(v4("127.0.0.1"), "127.0.0.1");
    CHECK(v4("127.255.0.3"), "127.255.0.3 (all of 127/8)");
    CHECK(v6("::1"), "::1");
    CHECK(v6("::ffff:127.0.0.1"), "::ffff:127.0.0.1 (v4-mapped, a dual-stack listener)");
    CHECK(v6("::ffff:127.9.9.9"), "::ffff:127.9.9.9 (all of mapped 127/8)");

    printf("not this host:\n");
    CHECK(!v4("10.0.0.1"), "10.0.0.1");
    CHECK(!v4("172.16.1.5"), "172.16.1.5");
    CHECK(!v4("128.0.0.1"), "128.0.0.1 (the octet above 127)");
    CHECK(!v6("::ffff:172.16.1.5"), "::ffff:172.16.1.5 (mapped LAN)");
    CHECK(!v6("::ffff:128.0.0.1"), "::ffff:128.0.0.1");
    CHECK(!v6("2001:db8::1"), "2001:db8::1");
    CHECK(!v6("fe80::1"), "fe80::1");
    CHECK(!v6("::"), ":: (unspecified)");
    CHECK(!v6("1::ffff:7f00:1"), "1::ffff:7f00:1 (mapped bytes without the zero prefix)");
    CHECK(!peer_is_loopback(NULL), "NULL");
    struct sockaddr_un un = { .sun_family = AF_UNIX };
    CHECK(!peer_is_loopback((const struct sockaddr *)&un), "AF_UNIX");

    /* A copy of sizeof(struct sockaddr) keeps the family and the first
     * eight address bytes; the rest reads as zero here (it is heap
     * garbage in the unpatched library). The check must fail closed. */
    struct sockaddr_in6 full = { .sin6_family = AF_INET6 };
    inet_pton(AF_INET6, "::ffff:127.0.0.1", &full.sin6_addr);
    struct sockaddr_in6 cut;
    memset(&cut, 0, sizeof(cut));
    memcpy(&cut, &full, sizeof(struct sockaddr));
    CHECK(!peer_is_loopback((const struct sockaddr *)&cut),
          "::ffff:127.0.0.1 cut to sizeof(struct sockaddr) is refused");

    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}

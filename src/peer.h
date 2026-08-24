/*
 * peer.h - is a socket peer this host?
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_PEER_H
#define FORGECTRL_PEER_H

#include <sys/socket.h>

/* 1 when sa is a loopback peer: 127.0.0.0/8, ::1, or the v4-mapped
 * ::ffff:127.0.0.0/8 a dual-stack listener reports for an IPv4 client.
 * sa must be the whole sockaddr of its family (a sockaddr_in6 for
 * AF_INET6); NULL, a truncated copy and any other family are not local. */
int peer_is_loopback(const struct sockaddr *sa);

#endif

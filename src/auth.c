/*
 * auth.c - forgectrl: HTTP access control
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The panel and its API are served in the clear on the LAN, so the
 * threat model is a hostile page in a same-LAN browser (CSRF), a
 * DNS-rebinding host, and any non-browser LAN client poking the port
 * directly. Three cheap layers close it:
 *
 *  1. Host must be an address literal (or localhost). A rebinding
 *     attacker reaches the daemon under their own hostname, which is not
 *     a literal - so the panel (and its embedded token) cannot be read
 *     back, and no state-changing call from that origin is honored.
 *  2. Sec-Fetch-Site, when the browser sends it, must be same-origin or
 *     none; a cross-site request is refused. An Origin header, when
 *     present, must itself be an address literal.
 *  3. A first-boot bearer token, stored 0600 in /data and embedded in
 *     the panel, is required on every state-changing endpoint. A CSRF
 *     page cannot read the token (same-origin policy hides the panel
 *     response), so it cannot forge an authorized call even against an
 *     old browser that omits Sec-Fetch-Site.
 *
 * The cooling report channel is authenticated differently: it only ever
 * comes from the controller on the same host, so it is restricted to a
 * loopback peer rather than the token.
 *
 * TLS and a network-level MITM are out of scope for this layer.
 */
#define _GNU_SOURCE
#include "auth.h"
#include "fflog.h"

#include <ctype.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define TOKEN_DIR   "/data/forgefirm"
#define TOKEN_FILE  TOKEN_DIR "/panel.token"
#define TOKEN_HEX   32                 /* 128 bits */
#define SWITCH_DEV  "/dev/input/event0"
#define SW_BIT_BUTTON 2

static char token[TOKEN_HEX + 1];

static void generate_token(void)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[TOKEN_HEX / 2];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
        if (fd >= 0)
            close(fd);
        /* Never ship a predictable token: without entropy the panel
         * must stay locked rather than fall back to something guessable. */
        token[0] = '\0';
        fflog(LOG_ERR, "auth: cannot read /dev/urandom - "
                       "panel token unavailable");
        return;
    }
    close(fd);
    for (size_t i = 0; i < sizeof(raw); i++) {
        token[i * 2] = hex[raw[i] >> 4];
        token[i * 2 + 1] = hex[raw[i] & 0xf];
    }
    token[TOKEN_HEX] = '\0';
}

void auth_init(void)
{
    mkdir(TOKEN_DIR, 0755);

    FILE *f = fopen(TOKEN_FILE, "r");
    if (f) {
        char buf[TOKEN_HEX + 4] = "";
        if (fgets(buf, sizeof(buf), f)) {
            size_t n = strspn(buf, "0123456789abcdef");
            if (n == TOKEN_HEX && (buf[n] == '\0' || buf[n] == '\n')) {
                memcpy(token, buf, TOKEN_HEX);
                token[TOKEN_HEX] = '\0';
            }
        }
        fclose(f);
        if (token[0])
            return;
    }

    generate_token();
    if (!token[0])
        return;

    int fd = open(TOKEN_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fflog(LOG_ERR, "auth: cannot persist panel token");
        return;
    }
    (void)!write(fd, token, TOKEN_HEX);
    (void)!write(fd, "\n", 1);
    close(fd);
    fflog(LOG_NOTICE, "auth: generated a new panel token");
}

const char *auth_token(void)
{
    return token;
}

/* Constant-time equality over the token length. */
static int token_eq(const char *a)
{
    if (!token[0] || !a)
        return 0;
    unsigned diff = 0;
    size_t i;
    for (i = 0; i < TOKEN_HEX && a[i]; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)token[i];
    /* length must match exactly */
    diff |= (unsigned)i ^ TOKEN_HEX;
    diff |= (unsigned char)a[i];        /* a[TOKEN_HEX] must be the terminator */
    return diff == 0;
}

/* A host string (Host header value, or an Origin's authority) is
 * accepted only as an address literal or localhost - never a DNS name,
 * which is the vehicle for a rebinding attack. */
static int host_is_literal(const char *h)
{
    if (!h || !*h)
        return 0;
    if (*h == '[')
        return 1;                       /* bracketed IPv6 literal */

    char hb[128];
    size_t o = 0;
    for (; h[o] && h[o] != ':' && o + 1 < sizeof(hb); o++)
        hb[o] = h[o];
    hb[o] = '\0';

    if (!strcmp(hb, "localhost"))
        return 1;
    if (!hb[0])
        return 0;
    for (size_t i = 0; hb[i]; i++)
        if (!isdigit((unsigned char)hb[i]) && hb[i] != '.')
            return 0;                   /* a letter => a name, not a literal */
    return 1;
}

/* Extract the authority (host[:port]) from a scheme://authority[/...]
 * Origin value into out. */
static void origin_authority(const char *origin, char *out, size_t len)
{
    out[0] = '\0';
    const char *p = strstr(origin, "://");
    if (!p)
        return;
    p += 3;
    size_t o = 0;
    for (; p[o] && p[o] != '/' && o + 1 < len; o++)
        out[o] = p[o];
    out[o] = '\0';
}

static int deny(struct _u_response *res, unsigned status, const char *msg)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    ulfius_set_string_body_response(res, status, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return 0;
}

/* Layer 1: Host literal + Sec-Fetch-Site + Origin. Returns 1 to allow. */
static int origin_ok(const struct _u_request *req)
{
    const char *host = u_map_get_case(req->map_header, "Host");
    if (!host_is_literal(host))
        return 0;

    const char *sfs = u_map_get_case(req->map_header, "Sec-Fetch-Site");
    if (sfs && strcmp(sfs, "same-origin") && strcmp(sfs, "none"))
        return 0;

    const char *origin = u_map_get_case(req->map_header, "Origin");
    if (origin && strcmp(origin, "null")) {
        char auth[128];
        origin_authority(origin, auth, sizeof(auth));
        if (!host_is_literal(auth))
            return 0;
    }
    return 1;
}

static int peer_is_loopback(const struct _u_request *req)
{
    const struct sockaddr *sa = req->client_address;
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
        if (!memcmp(a, lo, 16))
            return 1;                       /* ::1 */
        if (a[10] == 0xff && a[11] == 0xff && a[12] == 127)
            return 1;                       /* ::ffff:127.0.0.0/8 */
    }
    return 0;
}

int auth_read_ok(const struct _u_request *req, struct _u_response *res)
{
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    return 1;
}

int auth_write_permitted(const struct _u_request *req)
{
    if (!origin_ok(req))
        return 0;
    const char *tok = u_map_get_case(req->map_header, "X-ForgeFIRM-Token");
    if (!tok)
        tok = u_map_get(req->map_url, "token");
    return token_eq(tok);
}

int auth_write_ok(const struct _u_request *req, struct _u_response *res)
{
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    const char *tok = u_map_get_case(req->map_header, "X-ForgeFIRM-Token");
    if (!tok)
        tok = u_map_get(req->map_url, "token");
    if (!token_eq(tok))
        return deny(res, 403, "authentication required");
    return 1;
}

int auth_loopback_ok(const struct _u_request *req, struct _u_response *res)
{
    if (!origin_ok(req))
        return deny(res, 403, "request origin refused");
    if (!peer_is_loopback(req))
        return deny(res, 403, "loopback only");
    return 1;
}

int operator_present(void)
{
    uint8_t sw[2] = { 0 };
    int fd = open(SWITCH_DEV, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return 0;
    int ok = ioctl(fd, EVIOCGSW(sizeof(sw)), sw) >= 0;
    close(fd);
    return ok && (sw[SW_BIT_BUTTON / 8] & (1u << (SW_BIT_BUTTON % 8)));
}

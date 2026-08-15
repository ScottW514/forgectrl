/*
 * sanitize_test.c - host unit test for the log-export sanitizer
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A sanitized bundle is what people attach to public issue reports, so
 * a leak here is a privacy failure and an over-eager rule destroys the
 * evidence the bundle exists to carry. Fixtures cover both directions:
 * every identifying class must be redacted, and ordinary log text -
 * timestamps, kernel lines, paths, version strings, loopback - must
 * survive unchanged. Placeholders must be stable within a run and the
 * output must be idempotent (sanitizing it again changes nothing).
 */
#define _GNU_SOURCE
#include "../src/sanitize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static sanitizer_t *S;

/* Sanitize and compare; on mismatch print both. */
static void expect(const char *in, const char *want, const char *msg)
{
    char *out = san_line(S, in);
    int ok = out && !strcmp(out, want);
    CHECK(ok, msg);
    if (!ok)
        printf("       in:   %s\n       want: %s\n       got:  %s\n",
               in, want, out ? out : "(null)");
    free(out);
}

/* Sanitizing twice must be a fixed point. */
static void expect_idempotent(const char *in, const char *msg)
{
    char *once = san_line(S, in);
    char *twice = once ? san_line(S, once) : NULL;
    int ok = once && twice && !strcmp(once, twice);
    CHECK(ok, msg);
    if (!ok)
        printf("       once:  %s\n       twice: %s\n",
               once ? once : "(null)", twice ? twice : "(null)");
    free(once);
    free(twice);
}

int main(void)
{
    S = san_new();
    if (!S) {
        printf("san_new failed\n");
        return 2;
    }
    san_add_known(S, "SERIAL", "123456789");
    san_add_known(S, "HOSTNAME", "ABC-DEF");
    san_add_known(S, "GF_PASSWORD",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    san_add_known(S, "PANEL_TOKEN", "deadbeefdeadbeefdeadbeefdeadbeef");
    san_add_known(S, "SSID", "MyHomeWiFi");
    san_add_known(S, "PSK", "correct horse battery");
    san_add_known(S, "SHORT", "ab");            /* ignored: too short */
    san_add_known(S, "DIGITS", "1234");         /* ignored: short digits */

    printf("known values:\n");
    expect("machine serial 123456789 signed in",
           "machine serial <SERIAL> signed in", "serial replaced");
    expect("serial 1234567890 is a different number",
           "serial 1234567890 is a different number",
           "longer number containing the serial is left alone");
    expect("host ABC-DEF up, ABC-DEFG is another",
           "host <HOSTNAME> up, ABC-DEFG is another",
           "hostname replaced only at word boundaries");
    expect("password=0123456789abcdef0123456789abcdef0123456789abcdef"
           "0123456789abcdef",
           "password=<GF_PASSWORD>",
           "known credential wins over the generic value pattern");
    expect("X-ForgeFIRM-Token: deadbeefdeadbeefdeadbeefdeadbeef",
           "X-ForgeFIRM-Token: <PANEL_TOKEN>", "panel token by value");
    expect("wlan0: Trying to associate with SSID 'MyHomeWiFi'",
           "wlan0: Trying to associate with SSID '<SSID>'", "SSID by value");
    expect("psk correct horse battery ok",
           "psk <PSK> ok", "multi-word PSK by value");
    expect("abc 1234 ab", "abc 1234 ab", "too-short known values ignored");

    printf("credentials:\n");
    expect("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0In0."
           "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c",
           "Authorization: Bearer <TOKEN>", "bearer JWT");
    expect("hdr Basic dXNlcjpwYXNzd29yZA==", "hdr Basic <TOKEN>",
           "basic credential");
    expect("jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0In0."
           "SflKxwRJSMeKKF2QT4fwpMeJf36POk6yJV_adQssw5c end",
           "jwt <JWT> end", "bare JWT");
    expect("GET /ws?ws_token=abc123XYZ&foo=bar",
           "GET /ws?ws_token=<REDACTED>&foo=bar", "query token value");
    expect("cfg password: \"hunter2\" ok", "cfg password: \"<REDACTED>\" ok",
           "quoted password value");
    expect("psk=\"0123456789\"", "psk=\"<REDACTED>\"", "psk value");
    expect("re-authenticating for a fresh ws_token",
           "re-authenticating for a fresh ws_token",
           "key words without a value are left alone");
    expect("gfhome: starting homing session: /usr/bin/gfhome.py --home",
           "gfhome: starting homing session: /usr/bin/gfhome.py --home",
           "'session:' is not a credential key");
    expect("auth: cannot persist panel token",
           "auth: cannot persist panel token", "'auth:' prefix untouched");

    printf("addresses:\n");
    expect("Accepted none for root from 10.254.254.72 port 63997 ssh2",
           "Accepted none for root from <IP-1> port 63997 ssh2", "IPv4");
    expect("again 10.254.254.72 and 192.168.1.10:23 and 10.254.254.72",
           "again <IP-1> and <IP-2>:23 and <IP-1>",
           "stable numbering across lines");
    expect("listening on 127.0.0.1:8080 and 0.0.0.0",
           "listening on 127.0.0.1:8080 and 0.0.0.0",
           "loopback and unspecified kept");
    expect("version 6.12.3 and 1.2.3.4.5 chain",
           "version 6.12.3 and 1.2.3.4.5 chain",
           "version strings and 5-part dotted numbers kept");
    expect("wlan0: CTRL-EVENT-CONNECTED - Connection to aa:bb:cc:dd:ee:ff",
           "wlan0: CTRL-EVENT-CONNECTED - Connection to <MAC-1>", "MAC");
    expect("2026-08-15T16:35:44.123456-04:00 forgectrl[512] INFO super: "
           "started grbl controller (pid 3084)",
           "2026-08-15T16:35:44.123456-04:00 forgectrl[512] INFO super: "
           "started grbl controller (pid 3084)",
           "rsyslog timestamp line untouched");
    expect("Aug 15 16:35:44 glowforge kern.info kernel: [ 1078.045479] "
           "glowforge_cnc cnc: dms is active",
           "Aug 15 16:35:44 glowforge kern.info kernel: [ 1078.045479] "
           "glowforge_cnc cnc: dms is active", "kernel line untouched");
    expect("addr fe80::1234:5678:abcd:ef01 and [2001:db8::1]:514 and ::1",
           "addr <IP6-1> and [<IP6-2>]:514 and ::1", "IPv6, loopback kept");
    expect("full 2001:0db8:0000:0000:0000:ff00:0042:8329 x",
           "full <IP6-3> x", "eight-group IPv6");
    expect("time 16:35:44 dur 00:12 mac-ish 12:34:56:78",
           "time 16:35:44 dur 00:12 mac-ish 12:34:56:78",
           "colon-separated non-addresses kept");
    expect("user s.e.wiederhold@example.com and Other@Example.org, again "
           "s.e.wiederhold@example.com",
           "user <EMAIL-1> and <EMAIL-2>, again <EMAIL-1>", "e-mail");

    printf("blobs:\n");
    expect("sha 5b8e2fbd0a4d3c2b1a0f9e8d7c6b5a4f3e2d1c0b9a8f7e6d ok",
           "sha <HEX-1> ok", "long hex blob");
    expect("addr 0x1234abcd len 16 hex 0123456789abcdef",
           "addr 0x1234abcd len 16 hex 0123456789abcdef",
           "short hex left alone");
    expect("token AbCdEfGhIjKlMnOpQrStUvWxYz0123456789AbCdEfGh end",
           "token <B64-1> end", "long base64-like blob");
    expect("path /data/log/forgefirm/forgectrl/forgectrl.log "
           "/sys/bus/nvmem/devices/imx-ocotp0/nvmem",
           "path /data/log/forgefirm/forgectrl/forgectrl.log "
           "/sys/bus/nvmem/devices/imx-ocotp0/nvmem", "paths left alone");
    expect("word abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
           "word abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz",
           "long letters-only run left alone");

    printf("properties:\n");
    expect_idempotent("Accepted root from 10.254.254.72 mac aa:bb:cc:dd:ee:ff "
                      "serial 123456789 Bearer abcdefghijkl x@y.org "
                      "fe80::1 5b8e2fbd0a4d3c2b1a0f9e8d7c6b5a4f3e2d1c0b9a8f7e6d",
                      "output is a fixed point");
    CHECK(san_total(S) > 0, "redactions are counted");
    char *nl = san_line(S, "line with newline 10.0.0.9\n");
    CHECK(nl && !strcmp(nl, "line with newline <IP-3>\n"),
          "trailing newline preserved");
    free(nl);
    char *empty = san_line(S, "");
    CHECK(empty && !strcmp(empty, ""), "empty line");
    free(empty);

    printf("report:\n");
    san_report(S, stdout);
    san_free(S);

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

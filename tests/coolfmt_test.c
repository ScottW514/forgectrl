/*
 * coolfmt_test.c - host unit test for the /cool/status document
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The document is built from fixed-size fragments, and a fragment that
 * does not fit its buffer must turn into an empty one, never a cut-off
 * one: a client parses whatever the engine serves. Pinned here: the
 * widest legal rendering of every fragment (the far ends of the gate
 * table, a header-width floor, the longest names) fits the buffer the
 * engine uses; the widest document fits COOL_STATUS_JSON_MAX; the
 * whole thing parses as JSON; and an oversize value set is refused
 * rather than truncated.
 */
#include "../src/coolfmt.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

/* A small JSON acceptor: objects, arrays, strings (no escapes needed
 * here), numbers, true/false/null. Returns the position after the value
 * or -1. Enough to reject a document with a missing brace or a number
 * cut in half. */
static int value(const char *s, int i);

static int ws(const char *s, int i)
{
    while (s[i] == ' ' || s[i] == '\n' || s[i] == '\t') i++;
    return i;
}

static int string(const char *s, int i)
{
    if (s[i] != '"') return -1;
    for (i++; s[i] && s[i] != '"'; i++)
        if (s[i] == '\\') return -1;
    return s[i] == '"' ? i + 1 : -1;
}

static int number(const char *s, int i)
{
    int start = i;
    if (s[i] == '-') i++;
    while (s[i] >= '0' && s[i] <= '9') i++;
    if (s[i] == '.') {
        i++;
        int d = i;
        while (s[i] >= '0' && s[i] <= '9') i++;
        if (i == d) return -1;
    }
    return i > start && s[start] != '.' ? i : -1;
}

static int object(const char *s, int i)
{
    i = ws(s, i + 1);
    if (s[i] == '}') return i + 1;
    for (;;) {
        i = string(s, ws(s, i));
        if (i < 0) return -1;
        i = ws(s, i);
        if (s[i] != ':') return -1;
        i = value(s, ws(s, i + 1));
        if (i < 0) return -1;
        i = ws(s, i);
        if (s[i] == ',') { i++; continue; }
        return s[i] == '}' ? i + 1 : -1;
    }
}

static int array(const char *s, int i)
{
    i = ws(s, i + 1);
    if (s[i] == ']') return i + 1;
    for (;;) {
        i = value(s, ws(s, i));
        if (i < 0) return -1;
        i = ws(s, i);
        if (s[i] == ',') { i++; continue; }
        return s[i] == ']' ? i + 1 : -1;
    }
}

static int value(const char *s, int i)
{
    switch (s[i]) {
    case '{': return object(s, i);
    case '[': return array(s, i);
    case '"': return string(s, i);
    case 't': return strncmp(s + i, "true", 4) ? -1 : i + 4;
    case 'f': return strncmp(s + i, "false", 5) ? -1 : i + 5;
    case 'n': return strncmp(s + i, "null", 4) ? -1 : i + 4;
    default:  return number(s, i);
    }
}

static int is_json(const char *s)
{
    int i = value(s, ws(s, 0));
    return i >= 0 && s[ws(s, i)] == '\0';
}

int main(void)
{
    char lim[COOL_LIMITS_JSON_MAX], fg[COOL_FAN_GATES_JSON_MAX];
    char doc[COOL_STATUS_JSON_MAX];

    printf("limits\n");
    /* Today's defaults: the set that was cut at 160 bytes. */
    cool_limits_t def = {33.0, 0.0, 3700, 1800, 6000};
    CHECK(coolfmt_limits(lim, sizeof(lim), 33.0, 31.0, 38.0, 0, &def) == 0, "the default limits fit");
    CHECK(is_json(lim) && strstr(lim, "\"air_assist_min_rpm\":6000}"), "and end with the air assist floor, closed");
    /* The far ends of the gate table (gates.c): 60/59 C, floors 20000 /
     * 20000 / 30000, plus the header source. */
    cool_limits_t wide = {60.0, 60.0, 20000, 20000, 30000};
    CHECK(coolfmt_limits(lim, sizeof(lim), 60.0, 59.0, 70.0, 1, &wide) == 0, "the widest table values fit");
    CHECK(is_json(lim), "and parse");
    /* A header floor tightens past the table: a tach window of the
     * width the factory sends (64500), and an absurd one. */
    cool_limits_t hdr = {60.0, 60.0, 64500, 64500, 64500};
    CHECK(coolfmt_limits(lim, sizeof(lim), 60.0, 59.0, 70.0, 1, &hdr) == 0 && is_json(lim), "header-width floors fit");
    cool_limits_t absurd = {60.0, 60.0, 2147483647.0, 2147483647.0, 2147483647.0};
    CHECK(coolfmt_limits(lim, sizeof(lim), 60.0, 59.0, 70.0, 1, &absurd) == 0 && is_json(lim), "int32-width floors fit");
    CHECK(strstr(lim, "\"coolant_critical_c\":70.0,") != NULL, "the critical line is in the limits");
    char tiny[64];
    CHECK(coolfmt_limits(tiny, sizeof(tiny), 33.0, 31.0, 38.0, 0, &def) < 0 && !strcmp(tiny, "{}"), "a fragment that does not fit is refused, and left empty");
    size_t widest_limits = strlen(lim);

    printf("fan gates\n");
    coolfmt_fan_t fans[5] = {
        {"exhaust",    99999, 2147483647.0, "TRIPPED"},
        {"intake_1",   99999, 2147483647.0, "TRIPPED"},
        {"intake_2",   99999, 2147483647.0, "TRIPPED"},
        {"air_assist", 99999, 2147483647.0, "TRIPPED"},
        {"purge",      99999, 2147483647.0, "TRIPPED"},
    };
    CHECK(coolfmt_fan_gates(fg, sizeof(fg), fans, 5) == 0, "five fans at their widest fit");
    CHECK(is_json(fg) && strstr(fg, "\"purge\":{\"reading\":99999,\"floor\":2147483647,\"state\":\"TRIPPED\"}}"), "and parse, closed after the last row");
    CHECK(coolfmt_fan_gates(fg, sizeof(fg), fans, 0) == 0 && !strcmp(fg, "{}"), "no fans is the empty object");
    char small[100];
    CHECK(coolfmt_fan_gates(small, sizeof(small), fans, 5) < 0 && !strcmp(small, "{}"), "rows that do not fit are refused, not cut mid-row");
    coolfmt_fan_gates(fg, sizeof(fg), fans, 5);
    size_t widest_fans = strlen(fg);

    printf("document\n");
    char reason[COOL_REASON_MAX], gates_off[COOL_GATES_OFF_JSON_MAX];
    memset(reason, 'r', sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = '\0';
    /* The widest gates_off list the engine can publish is every gate
     * key; pad a valid array out to the buffer's full width. */
    snprintf(gates_off, sizeof(gates_off), "[\"%0*d\"]", (int)sizeof(gates_off) - 5, 0);
    coolfmt_status_t st = {
        .phase = "cooldown", .verdict = "OVERTEMP", .reason = reason,
        .fire_watch = "armed", .fire_ok = 0, .hold = 0, .armed = 0,
        .down_c = -273.15, .up_c = -273.15, .report_age_s = 99999999.9,
        .gates_off = gates_off, .limits = lim, .fan_gates = fg,
    };
    CHECK(coolfmt_status(doc, sizeof(doc), &st) == 0, "the widest document fits COOL_STATUS_JSON_MAX");
    CHECK(is_json(doc), "and parses");
    size_t scalars = strlen(doc) - strlen(reason) - strlen(gates_off) - widest_limits - widest_fans;
    printf("  widest: limits %zu, fan gates %zu, scalars+punctuation %zu, document %zu of %d\n",
           widest_limits, widest_fans, scalars, strlen(doc), COOL_STATUS_JSON_MAX);
    CHECK(scalars < COOL_STATUS_JSON_SCALARS, "the scalar allowance covers the widest scalars");
    CHECK(coolfmt_status(doc, 512, &st) < 0, "a 512-byte buffer is refused, not served cut off");

    /* The document parses with every fragment at its empty fallback. */
    st.gates_off = "[]"; st.limits = "{}"; st.fan_gates = "{}"; st.reason = "";
    CHECK(coolfmt_status(doc, sizeof(doc), &st) == 0 && is_json(doc), "empty fragments still make a document");

    printf("json acceptor\n");
    CHECK(!is_json("{\"a\":{\"b\":60,\"c\":{}}"), "the acceptor rejects the cut document this test exists for");
    CHECK(!is_json("{\"a\":1"), "and a missing brace");
    CHECK(is_json("{\"a\":[1,-2.5,\"x\",true,null],\"b\":{}}"), "and accepts a normal one");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

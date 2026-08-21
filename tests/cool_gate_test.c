/*
 * cool_gate_test.c - host unit test for the gate-settings table
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The cooling gates are plain settings whose far end means off, and
 * every consumer (the validators, the engine, the settings reply, the
 * panel) reads one table. This test pins the table's shape (default
 * inside band inside legal range, off end where the contract says),
 * the parse rules the validators rely on, the state classification,
 * and the two JSON encodings the panel and /status consume.
 */
#include "../src/gates.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static double value_default(const gate_setting_t *g, void *ctx)
{
    (void)ctx;
    return g->def;
}

static double value_from_table(const gate_setting_t *g, void *ctx)
{
    const char *const *kv = ctx;   /* key, value, key, value, ..., NULL */
    for (size_t i = 0; kv[i]; i += 2)
        if (!strcmp(kv[i], g->key))
            return strtod(kv[i + 1], NULL);
    return g->def;
}

int main(void)
{
    size_t n;
    const gate_setting_t *t = gate_settings(&n);

    printf("table shape\n");
    CHECK(n == 9, "nine gate settings");
    for (size_t i = 0; i < n; i++) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: lo <= band_lo <= def <= band_hi <= hi", t[i].key);
        CHECK(t[i].lo <= t[i].band_lo && t[i].band_lo <= t[i].def &&
              t[i].def <= t[i].band_hi && t[i].band_hi <= t[i].hi, msg);
        snprintf(msg, sizeof(msg), "%s: default is not the off end", t[i].key);
        CHECK(gate_state(&t[i], t[i].def) == Gate_Ok, msg);
    }
    const gate_setting_t *tmax = gate_setting_find("cool_temp_max");
    const gate_setting_t *tres = gate_setting_find("cool_temp_resume");
    const gate_setting_t *fcs = gate_setting_find("cool_flow_check_s");
    const gate_setting_t *rise = gate_setting_find("cool_flow_rise");
    CHECK(tmax && tres && fcs && rise, "every key resolves");
    CHECK(!gate_setting_find("cool_recheck_s"), "a non-gate key does not resolve");
    CHECK(!gate_setting_find(NULL), "NULL does not resolve");
    CHECK(tmax->gate && !strcmp(tmax->gate, "coolant_max") && tmax->off_end > 0,
          "the coolant ceiling is the coolant_max gate, off at its top");
    CHECK(fcs->gate && !strcmp(fcs->gate, "flow") && fcs->off_end < 0 && fcs->lo == 0.0,
          "the flow window is the flow gate, off at zero");
    CHECK(!tres->gate && tres->off_end == 0, "the resume gate is not a gate of its own");
    CHECK(!rise->gate && rise->off_end == 0, "the flow rise is not a gate of its own");
    CHECK(tres->hi < tmax->hi, "the resume range tops out under the ceiling range");
    CHECK(tmax->band_hi == 38.0, "the ceiling band tops out at the former hard cap");
    const gate_setting_t *exh = gate_setting_find("cool_tach_exhaust_min_rpm");
    const gate_setting_t *inl = gate_setting_find("cool_tach_intake_min_rpm");
    const gate_setting_t *air = gate_setting_find("cool_tach_air_assist_min_rpm");
    const gate_setting_t *prg = gate_setting_find("cool_purge_min_current");
    const gate_setting_t *grc = gate_setting_find("cool_fan_grace_s");
    CHECK(exh && inl && air && prg && grc, "every airflow key resolves");
    CHECK(exh->gate && !strcmp(exh->gate, "exhaust") && exh->off_end < 0 && exh->lo == 0.0,
          "the exhaust floor is the exhaust gate, off at zero");
    CHECK(inl->gate && !strcmp(inl->gate, "intake") && inl->off_end < 0,
          "the intake floor is the intake gate, off at zero");
    CHECK(air->gate && !strcmp(air->gate, "air_assist") && air->off_end < 0,
          "the air-assist floor is the air_assist gate, off at zero");
    CHECK(prg->gate && !strcmp(prg->gate, "purge") && prg->off_end < 0 && prg->hi == 1023.0,
          "the purge current floor is the purge gate, off at zero, capped at the ADC rail");
    CHECK(!grc->gate && grc->off_end == 0, "the grace window is not a gate of its own");
    CHECK(gate_state(exh, 0.0) == Gate_Off && gate_state(exh, 1.0) == Gate_Warn &&
          gate_state(exh, 3700.0) == Gate_Ok, "an exhaust floor of zero is off, of one is warned");

    printf("parse\n");
    double v = 0;
    CHECK(gate_parse(tmax, "33", &v) && v == 33.0, "a plain number inside the range");
    CHECK(gate_parse(tmax, "60", &v) && v == 60.0, "the top of the range is legal");
    CHECK(gate_parse(tmax, "5", &v) && v == 5.0, "the bottom of the range is legal");
    CHECK(!gate_parse(tmax, "60.1", &v), "just past the top is not");
    CHECK(!gate_parse(tmax, "4.9", &v), "just under the bottom is not");
    CHECK(!gate_parse(tmax, "", &v), "empty is not a value");
    CHECK(!gate_parse(tmax, "abc", &v), "garbage is not a value");
    CHECK(!gate_parse(tmax, "33x", &v), "trailing text is not a value");
    CHECK(!gate_parse(tmax, "nan", &v), "nan is not a value");
    CHECK(!gate_parse(tmax, "inf", &v), "inf is not a value");
    CHECK(gate_parse(fcs, "0", &v) && v == 0.0, "the flow window accepts zero");
    CHECK(!gate_parse(fcs, "-1", &v), "the flow window rejects negatives");
    CHECK(gate_parse(rise, "40", &v), "the flow rise accepts its wide top");
    CHECK(!gate_parse(NULL, "1", &v), "a NULL row parses nothing");

    printf("state\n");
    CHECK(gate_state(tmax, 33.0) == Gate_Ok, "ceiling at default: ok");
    CHECK(gate_state(tmax, 25.0) == Gate_Ok, "ceiling at band low edge: ok");
    CHECK(gate_state(tmax, 38.0) == Gate_Ok, "ceiling at band high edge: ok");
    CHECK(gate_state(tmax, 24.9) == Gate_Warn, "ceiling under the band: warn");
    CHECK(gate_state(tmax, 38.1) == Gate_Warn, "ceiling over the band: warn");
    CHECK(gate_state(tmax, 59.9) == Gate_Warn, "ceiling just under the top: warn, not off");
    CHECK(gate_state(tmax, 60.0) == Gate_Off, "ceiling at the top: off");
    CHECK(gate_state(tmax, 75.0) == Gate_Off, "ceiling past the top (env override): off");
    CHECK(gate_state(fcs, 50.0) == Gate_Ok, "flow window at default: ok");
    CHECK(gate_state(fcs, 1.0) == Gate_Warn, "flow window of one second: warn, not off");
    CHECK(gate_state(fcs, 0.0) == Gate_Off, "flow window of zero: off");
    CHECK(gate_state(fcs, 300.0) == Gate_Warn, "flow window at its top: warn (the off end is low)");
    CHECK(gate_state(tres, 5.0) == Gate_Warn, "resume at its bottom: warn, never off");
    CHECK(gate_state(tres, 59.0) == Gate_Warn, "resume at its top: warn, never off");
    CHECK(gate_state(rise, 40.0) == Gate_Warn, "rise at its top: warn, never off");
    CHECK(!strcmp(gate_state_name(Gate_Ok), "ok") &&
          !strcmp(gate_state_name(Gate_Warn), "warn") &&
          !strcmp(gate_state_name(Gate_Off), "off"), "state names");

    printf("effective\n");
    int from = -1;
    CHECK(gate_effective(33.0, 30.0, 0, &from) == 30.0 && from == 1, "a stricter header ceiling applies");
    CHECK(gate_effective(33.0, 33.0, 0, &from) == 33.0 && from == 0, "an equal header ceiling is the local one");
    CHECK(gate_effective(33.0, 40.0, 0, &from) == 33.0 && from == 0, "a looser header ceiling is ignored");
    CHECK(gate_effective(33.0, -1.0, 0, &from) == 33.0 && from == 0, "an absent header ceiling (-1) is ignored");
    CHECK(gate_effective(33.0, 0.0, 0, &from) == 33.0 && from == 0, "a zero header ceiling is absent");
    CHECK(gate_effective(33.0, NAN, 0, &from) == 33.0 && from == 0, "a NaN header ceiling is absent");
    CHECK(gate_effective(33.0, INFINITY, 0, &from) == 33.0 && from == 0, "an infinite header ceiling is absent");
    CHECK(gate_effective(900.0, 1200.0, 1, &from) == 1200.0 && from == 1, "a stricter header floor applies");
    CHECK(gate_effective(900.0, 600.0, 1, &from) == 900.0 && from == 0, "a looser header floor is ignored");
    CHECK(gate_effective(0.0, 116.0, 1, &from) == 116.0 && from == 1, "a floor with no local value takes the header's");
    CHECK(gate_effective(0.0, -1.0, 1, &from) == 0.0 && from == 0, "a floor with nothing anywhere stays zero");
    CHECK(gate_effective(33.0, 30.0, 0, NULL) == 30.0, "a NULL from_header is accepted");

    printf("json\n");
    char buf[2048];
    int r = gates_json(buf, sizeof(buf), value_default, NULL);
    CHECK(r > 0 && buf[0] == '{' && buf[r - 1] == '}', "gates_json is an object");
    CHECK(strstr(buf, "\"cool_temp_max\":{\"gate\":\"coolant_max\",\"def\":33,\"lo\":5,"
                      "\"hi\":60,\"band\":[25,38],\"off\":\"high\",\"value\":33,"
                      "\"state\":\"ok\"}") != NULL,
          "the ceiling row carries gate, range, band, off end, value and state");
    CHECK(strstr(buf, "\"cool_temp_resume\":{\"gate\":null,") != NULL,
          "a non-gate row carries gate:null");
    CHECK(strstr(buf, "\"cool_flow_rise\":{\"gate\":null,\"def\":14.4,") != NULL,
          "a fractional default prints without trailing zeros");
    CHECK(strstr(buf, "\"cool_flow_check_s\":{\"gate\":\"flow\",\"def\":50,\"lo\":0,"
                      "\"hi\":300,\"band\":[30,120],\"off\":\"low\",") != NULL,
          "the flow row says its off end is low");
    CHECK(strstr(buf, "\"cool_tach_exhaust_min_rpm\":{\"gate\":\"exhaust\",\"def\":3700,") != NULL,
          "the exhaust row is there");
    CHECK(gates_json(buf, 16, value_default, NULL) == -1, "a short buffer reports -1");

    const char *all_default[] = { NULL };
    r = gates_off_json(buf, sizeof(buf), value_from_table, (void *)all_default);
    CHECK(r == 0 && !strcmp(buf, "[]"), "defaults: no gate off");
    const char *ceiling_off[] = { "cool_temp_max", "60", NULL };
    r = gates_off_json(buf, sizeof(buf), value_from_table, (void *)ceiling_off);
    CHECK(r == 1 && !strcmp(buf, "[\"coolant_max\"]"), "ceiling at 60: coolant_max off");
    const char *both_off[] = { "cool_temp_max", "60", "cool_flow_check_s", "0", NULL };
    r = gates_off_json(buf, sizeof(buf), value_from_table, (void *)both_off);
    CHECK(r == 2 && !strcmp(buf, "[\"coolant_max\",\"flow\"]"), "both off, in table order");
    const char *fans_off[] = { "cool_tach_exhaust_min_rpm", "0", "cool_purge_min_current", "0",
                               "cool_fan_grace_s", "0", NULL };
    r = gates_off_json(buf, sizeof(buf), value_from_table, (void *)fans_off);
    CHECK(r == 2 && !strcmp(buf, "[\"exhaust\",\"purge\"]"),
          "fan floors at zero are off; a zero grace is not a gate");
    const char *warn_only[] = { "cool_temp_max", "45", "cool_flow_rise", "40", NULL };
    r = gates_off_json(buf, sizeof(buf), value_from_table, (void *)warn_only);
    CHECK(r == 0 && !strcmp(buf, "[]"), "warned values are not off");
    CHECK(gates_off_json(buf, 2, value_from_table, (void *)both_off) == -1,
          "a short buffer reports -1");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

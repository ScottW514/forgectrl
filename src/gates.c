/*
 * gates.c - gate settings: legal range, recommended band, off end
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "gates.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One row per gate setting. The defaults are the factory's coolant
 * window (cool.c carries the measured rationale); the bands are the
 * defaults with the margin one machine's loop was seen to need; the
 * legal ranges are wide on purpose, because a field machine whose
 * sensors read out of line with this bench changes the number rather
 * than patching the firmware, and the far end is how it turns a gate
 * off while the defaults catch up.
 *
 * cool_temp_max: the run ceiling on the upstream coolant sensor. 60 C
 *   is past anything the loop reaches before the tube is already
 *   damaged, so a ceiling there is a gate that never trips.
 * cool_temp_resume: the resume gate under the ceiling. Not a gate of
 *   its own (it follows the ceiling), so no off end; it is warned
 *   outside its band and kept below the ceiling by the settings
 *   cross-check.
 * cool_temp_critical_c: the critical line above the ceiling, a fault
 *   with no resume in the run session where the ceiling is a pause.
 *   Kept above the ceiling by the cross-check while the ceiling is a
 *   gate (a ceiling at its off end leaves the line standing alone);
 *   70 C is past anything the loop reaches, so a line there is the
 *   gate off.
 * cool_temp_min: the coolant floor under the ceiling, a fire gate with
 *   a degree of hysteresis (cool.c). Cold coolant shocks a tube and a
 *   TEC pulling under the dew point condenses on it; the factory's own
 *   floor rides in the pulse header (CMrn) and can only raise this one.
 *   Zero is the gate off.
 * cool_temp_start: the warm-up gate. A run session that opens with the
 *   coolant under it holds with the loop heater on and the fans idle
 *   until the reading reaches the gate, then runs and verifies flow.
 *   Zero is the gate off. Kept between the floor and the ceiling by the
 *   settings cross-check.
 * cool_fire_q1_alert .. cool_fire_q2_critical: the lid-IR fire watch,
 *   the factory's shape. The four channels sorted ascending are the
 *   quartiles; a sustained first or second quartile over its alert is
 *   the pause tier, over its critical the fail tier. The defaults are
 *   the thresholds the factory ships in every pulse header (quartiles
 *   three and four it leaves at zero, off), and they sit far above a
 *   fully lit lid lamp (161 to 177 counts) plus its drift: the watch
 *   catches a developed flame, not a candle. Zero is that tier off.
 * cool_tec_on_c / cool_tec_off_c: the TEC's hysteresis pair on the
 *   upstream reading (on above, off below), used only when
 *   cool_tec_present is set. Not gates of their own (nothing trips);
 *   the settings cross-check keeps off under on, and the engine's
 *   runtime clamp holds the TEC off within a degree of the floor.
 *   The factory turns a Pro's TEC on above its own threshold; what a
 *   Pro sets is unknown (no Pro capture), so these defaults are chosen:
 *   around the observed Pro loop point, above the warm-up gate, above
 *   the dew point of an ordinary room.
 * cool_accel_x_alert .. cool_accel_abort: the head-accelerometer
 *   crash watch, the factory's shape on the LIS2HH12's own interrupt
 *   generators. Values are IG threshold register units at the +/-4 g
 *   run full scale (LSB = full scale/256, ~15.6 mg): the per-axis
 *   alerts are the pause tier (IG1), the shared abort the fail tier
 *   (IG2). The defaults are the factory's own cut and travel header
 *   values (x 132 ~2.06 g, y 112 ~1.75 g, abort 133 ~2.08 g); normal
 *   commanded motion reads under 0.2 g and a rail strike 1.8 g and
 *   up. Z is never armed: gravity rides it. Zero is that tier off.
 * cool_flow_check_s: the flow interrogation window. Zero is no
 *   interrogation at all, the one off-by-value the engine has always
 *   had; the band is the window the characterization found useful.
 * cool_flow_rise: the downstream rise that means stagnant coolant. Set
 *   from flow calibrate; the bench's no-flow rise is about 16 C, so a
 *   value above the band can never fault and is warned as such. Not a
 *   gate of its own (flow is), so no off end.
 * cool_tach_*_min_rpm: the airflow floors, in rpm at run duty, 55
 *   percent of the steady speed the bench machine's fans reach at the
 *   run profile (exhaust 11640, the intakes 4160, the air assist
 *   11050; the bands are 50 to 60 percent); zero is the gate off. A
 *   pulse header's tach window can raise a floor for a job, never
 *   lower it.
 * cool_purge_min_current: the purge-air fan has no tachometer; its
 *   current reads about 1 off and about 630 on, and the floor sits
 *   between. Zero is the gate off.
 * cool_fan_grace_s: the spin-up window after the run profile is
 *   written, during which no floor counts. Not a gate, so no off end. */
static const gate_setting_t table[] = {
    { "cool_temp_max",              "coolant_max", 33.0,  5.0,    60.0, 25.0,  38.0, +1 },
    { "cool_temp_resume",           NULL,          31.0,  5.0,    59.0, 20.0,  36.0,  0 },
    { "cool_temp_critical_c",       "coolant_critical", 38.0, 6.0, 70.0, 36.0, 45.0, +1 },
    { "cool_temp_min",              "coolant_min",  5.0,  0.0,    40.0,  3.0,   8.0, -1 },
    { "cool_temp_start",            "warm_up",     16.0,  0.0,    40.0, 12.0,  20.0, -1 },
    { "cool_tec_on_c",              NULL,          20.0,  6.0,    32.0, 18.0,  24.0,  0 },
    { "cool_tec_off_c",             NULL,          18.0,  5.0,    31.0, 16.0,  22.0,  0 },
    { "cool_fire_q1_alert",         "flame_q1_alert",    275.0, 0.0, 1023.0, 250.0,  450.0, -1 },
    { "cool_fire_q1_critical",      "flame_q1_critical", 688.0, 0.0, 1023.0, 500.0, 1023.0, -1 },
    { "cool_fire_q2_alert",         "flame_q2_alert",    374.0, 0.0, 1023.0, 300.0,  500.0, -1 },
    { "cool_fire_q2_critical",      "flame_q2_critical", 1022.0, 0.0, 1023.0, 500.0, 1023.0, -1 },
    { "cool_accel_x_alert",         "crash_x_alert", 132.0, 0.0, 255.0, 100.0, 170.0, -1 },
    { "cool_accel_y_alert",         "crash_y_alert", 112.0, 0.0, 255.0,  85.0, 145.0, -1 },
    { "cool_accel_abort",           "crash_abort",   133.0, 0.0, 255.0, 100.0, 170.0, -1 },
    { "cool_flow_check_s",          "flow",        50.0,  0.0,   300.0, 30.0, 120.0, -1 },
    { "cool_flow_rise",             NULL,          14.4,  1.0,    40.0,  8.0,  16.0,  0 },
    { "cool_tach_exhaust_min_rpm",  "exhaust",   6400.0,  0.0, 20000.0, 5800.0, 7000.0, -1 },
    { "cool_tach_intake_min_rpm",   "intake",    2290.0,  0.0, 20000.0, 2100.0, 2500.0, -1 },
    { "cool_tach_air_assist_min_rpm", "air_assist", 6000.0, 0.0, 30000.0, 5500.0, 6600.0, -1 },
    { "cool_purge_min_current",     "purge",      300.0,  0.0,  1023.0, 150.0, 500.0, -1 },
    { "cool_fan_grace_s",           NULL,          15.0,  0.0,   120.0,  5.0,  30.0,  0 },
};

const gate_setting_t *gate_settings(size_t *n)
{
    if (n)
        *n = sizeof(table) / sizeof(table[0]);
    return table;
}

const gate_setting_t *gate_setting_find(const char *key)
{
    if (!key)
        return NULL;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (!strcmp(table[i].key, key))
            return &table[i];
    return NULL;
}

int gate_parse(const gate_setting_t *g, const char *text, double *out)
{
    if (!g || !text || !text[0])
        return 0;
    char *end;
    double v = strtod(text, &end);
    if (end == text || *end != '\0' || !isfinite(v))
        return 0;
    if (v < g->lo || v > g->hi)
        return 0;
    if (out)
        *out = v;
    return 1;
}

gate_state_t gate_state(const gate_setting_t *g, double v)
{
    if (g->off_end < 0 && v <= g->lo)
        return Gate_Off;
    if (g->off_end > 0 && v >= g->hi)
        return Gate_Off;
    if (v < g->band_lo || v > g->band_hi)
        return Gate_Warn;
    return Gate_Ok;
}

const char *gate_state_name(gate_state_t s)
{
    switch (s) {
    case Gate_Warn: return "warn";
    case Gate_Off:  return "off";
    default:        return "ok";
    }
}

double gate_effective(double local, double header, int is_floor, int *from_header)
{
    int use = isfinite(header) && header > 0.0 &&
              (is_floor ? header > local : header < local);
    if (from_header)
        *from_header = use;
    return use ? header : local;
}

int gate_floor_trip(int tripped, double up_c, double floor_c, double hyst_c)
{
    return up_c < (tripped ? floor_c + hyst_c : floor_c);
}

/* %g keeps 33 as 33 and 14.4 as 14.4 without trailing zeros. */
static int put(char *buf, size_t len, size_t *off, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, len - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= len - *off)
        return -1;
    *off += (size_t)n;
    return 0;
}

int gates_json(char *buf, size_t len, gate_value_fn value_of, void *ctx)
{
    size_t off = 0, n;
    const gate_setting_t *t = gate_settings(&n);
    if (!buf || len < 2)
        return -1;
    if (put(buf, len, &off, "{"))
        return -1;
    for (size_t i = 0; i < n; i++) {
        double v = value_of(&t[i], ctx);
        if (put(buf, len, &off,
                "%s\"%s\":{\"gate\":%s%s%s,\"def\":%g,\"lo\":%g,\"hi\":%g,"
                "\"band\":[%g,%g],\"off\":\"%s\",\"value\":%g,\"state\":\"%s\"}",
                i ? "," : "", t[i].key,
                t[i].gate ? "\"" : "", t[i].gate ? t[i].gate : "null",
                t[i].gate ? "\"" : "",
                t[i].def, t[i].lo, t[i].hi, t[i].band_lo, t[i].band_hi,
                t[i].off_end < 0 ? "low" : t[i].off_end > 0 ? "high" : "none",
                v, gate_state_name(gate_state(&t[i], v))))
            return -1;
    }
    if (put(buf, len, &off, "}"))
        return -1;
    return (int)off;
}

int gates_off_json(char *buf, size_t len, gate_value_fn value_of, void *ctx)
{
    size_t off = 0, n;
    int count = 0;
    const gate_setting_t *t = gate_settings(&n);
    if (!buf || len < 3)
        return -1;
    if (put(buf, len, &off, "["))
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (!t[i].gate)
            continue;
        if (gate_state(&t[i], value_of(&t[i], ctx)) != Gate_Off)
            continue;
        if (put(buf, len, &off, "%s\"%s\"", count ? "," : "", t[i].gate))
            return -1;
        count++;
    }
    if (put(buf, len, &off, "]"))
        return -1;
    return count;
}

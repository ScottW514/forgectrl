/*
 * coolfmt.c - the /cool/status document: pure formatters, sized fragments
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#include "coolfmt.h"

#include <stdio.h>

/* snprintf reports the length it wanted; anything at or past len was
 * cut. A cut fragment becomes the empty one so the document stays JSON. */
static int fit(char *buf, size_t len, int n, const char *empty)
{
    if (n >= 0 && (size_t)n < len)
        return 0;
    snprintf(buf, len, "%s", empty);
    return -1;
}

int coolfmt_limits(char *buf, size_t len, double max_c, double resume_c,
                   double critical_c, int from_header, const cool_limits_t *eff)
{
    int n = snprintf(buf, len,
                     "{\"coolant_max_c\":%.1f,\"coolant_resume_c\":%.1f,"
                     "\"coolant_critical_c\":%.1f,"
                     "\"coolant_source\":\"%s\",\"coolant_min_c\":%.1f,"
                     "\"exhaust_min_rpm\":%.0f,\"intake_min_rpm\":%.0f,"
                     "\"air_assist_min_rpm\":%.0f}",
                     max_c, resume_c, critical_c, from_header ? "header" : "local",
                     eff->coolant_min_c, eff->exhaust_min_rpm,
                     eff->intake_min_rpm, eff->air_assist_min_rpm);
    return fit(buf, len, n, "{}");
}

int coolfmt_fan_gates(char *buf, size_t len, const coolfmt_fan_t *fans, size_t n)
{
    size_t off = 0;
    int w = snprintf(buf, len, "{");
    if (fit(buf, len, w, "{}") < 0)
        return -1;
    off += (size_t)w;
    for (size_t i = 0; i < n; i++) {
        w = snprintf(buf + off, len - off,
                     "%s\"%s\":{\"reading\":%.0f,\"floor\":%.0f,\"state\":\"%s\"}",
                     i ? "," : "", fans[i].name, fans[i].reading, fans[i].floor,
                     fans[i].state);
        if (w < 0 || (size_t)w >= len - off) {
            snprintf(buf, len, "{}");
            return -1;
        }
        off += (size_t)w;
    }
    w = snprintf(buf + off, len - off, "}");
    if (w < 0 || (size_t)w >= len - off) {
        snprintf(buf, len, "{}");
        return -1;
    }
    return 0;
}

int coolfmt_status(char *buf, size_t len, const coolfmt_status_t *s)
{
    int n = snprintf(buf, len,
                     "{\"phase\":\"%s\",\"verdict\":\"%s\",\"fire_ok\":%s,"
                     "\"hold\":%s,\"reason\":\"%s\",\"down_c\":%.2f,\"up_c\":%.2f,"
                     "\"report_age_s\":%.1f,\"armed\":%s,\"fire_watch\":\"%s\","
                     "\"accel_watch\":\"%s\","
                     "\"gates_off\":%s,\"limits\":%s,\"fan_gates\":%s}",
                     s->phase, s->verdict, s->fire_ok ? "true" : "false",
                     s->hold ? "true" : "false", s->reason, s->down_c, s->up_c,
                     s->report_age_s, s->armed ? "true" : "false", s->fire_watch,
                     s->accel_watch ? s->accel_watch : "watch",
                     s->gates_off, s->limits, s->fan_gates);
    return n < 0 || (size_t)n >= len ? -1 : 0;
}

int coolfmt_armed(int reported, double report_age_s, double timeout_s)
{
    return reported && report_age_s >= 0 && report_age_s <= timeout_s;
}

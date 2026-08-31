/*
 * gates.h - gate settings: legal range, recommended band, off end
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A gate is a comparison the cooling engine makes against a setting: a
 * ceiling the coolant must stay under, a window the flow check runs
 * for. Every such setting is a plain number on the Machine tab with a
 * wide legal range, and the far end of that range turns the gate off by
 * value: a floor of zero never trips, a ceiling at its maximum never
 * trips. There is no separate switch. Around the shipped default sits a
 * recommended band; a value outside it is legal and warned about, a
 * value at the off end is legal and reported as the gate being off.
 *
 * This module is pure (no I/O) so the validators, the engine, the
 * settings reply and the host tests all read one table.
 */
#ifndef FORGECTRL_GATES_H
#define FORGECTRL_GATES_H

#include <stddef.h>

typedef enum {
    Gate_Ok = 0,     /* inside the recommended band */
    Gate_Warn,       /* legal, outside the band */
    Gate_Off         /* at the off end: the gate never trips */
} gate_state_t;

typedef struct {
    const char *key;       /* settings key (cool_*) */
    const char *gate;      /* gate name as /status reports it, or NULL
                            * for a key that tunes a gate without being
                            * one (the resume gate, the flow rise) */
    double def;            /* compiled default */
    double lo, hi;         /* legal range, inclusive */
    double band_lo;        /* recommended band, inclusive */
    double band_hi;
    int off_end;           /* -1: lo is off; +1: hi is off; 0: none */
} gate_setting_t;

/* The table, and a lookup by settings key (NULL if not a gate setting). */
const gate_setting_t *gate_settings(size_t *n);
const gate_setting_t *gate_setting_find(const char *key);

/* Parse a setting's text as a number inside its legal range. Returns 1
 * and stores the value, or 0 for anything else (garbage, out of range,
 * trailing text). An empty string is not a value. */
int gate_parse(const gate_setting_t *g, const char *text, double *out);

/* Classify a value. Off wins over warn; a value outside the legal
 * range classifies by the same rules (the validators keep it out of the
 * settings file, but an env override can still put it in the engine). */
gate_state_t gate_state(const gate_setting_t *g, double v);
const char *gate_state_name(gate_state_t s);     /* "ok" "warn" "off" */

/* The effective limit for one gate: the stricter of the locally
 * configured value and what a job's header asked for. A ceiling can
 * only come down, a floor can only go up; a header value that is
 * absent (not finite, or not above zero) or looser leaves the local
 * value standing. Returns the effective value and, through *from_header,
 * whether the header's value is the one in force. */
double gate_effective(double local, double header, int is_floor, int *from_header);

/* The coolant floor's hysteresis: tripped under the floor, it stays
 * tripped until the reading is hyst_c above the floor. Returns the new
 * tripped state. */
int gate_floor_trip(int tripped, double up_c, double floor_c, double hyst_c);

/* Value source for the JSON helpers: the engine passes its resolved
 * tunables, the settings reply passes the file's values. */
typedef double (*gate_value_fn)(const gate_setting_t *g, void *ctx);

/* {"cool_temp_max":{"gate":"coolant_max","def":33,"lo":5,"hi":60,
 *  "band":[25,38],"off":"high","value":33,"state":"ok"},...}
 * Returns the length written, or -1 if it did not fit. */
int gates_json(char *buf, size_t len, gate_value_fn value_of, void *ctx);

/* ["coolant_max","flow"]: the gates whose setting is at its off end.
 * Returns the count of gates off, or -1 if the buffer did not fit. */
int gates_off_json(char *buf, size_t len, gate_value_fn value_of, void *ctx);

#endif

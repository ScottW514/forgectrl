/*
 * status.h - machine operational status (see status.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_STATUS_H
#define FORGECTRL_STATUS_H

#include <stddef.h>

/* Build the /status JSON (motion state, position when homed, fans,
 * coolant, switches) into buf. Always succeeds; unreadable sources are
 * omitted or defaulted. Needs ~512 bytes. extra, when not NULL, is
 * one or more ready-made `"key":value` members (comma-separated, no
 * trailing comma) spliced in by the caller that owns them: this file
 * reads sysfs only, so engine state arrives this way. */
int machine_status_json(char *buf, size_t len, const char *extra);

/* True only when the motion driver reports the idle state. Fails closed:
 * any read failure - including fd exhaustion under a connection flood
 * (EMFILE) - reports not-idle, so a destructive action (flash, mode
 * switch, diag) is never permitted on a bad read. Settings changes and
 * takeovers are only allowed while idle. */
int machine_is_idle(void);

/* True only when the lid is positively reported closed (the EV_SW
 * `doors` bit, the series combination of both lid switches that the
 * hardware safety chain itself uses). Fails closed: any read failure
 * reports NOT closed, so the camera privacy gate keeps the sensors dark
 * rather than capturing on a bad read. */
int machine_lid_closed(void);

/* Factory coolant-thermistor conversion (shared with the diagnostics
 * runner). Raw 10-bit ADC count -> degrees C; out-of-range input maps
 * to -273.15. */
double coolant_degc(long raw);

/* The board temperatures, watched and not gated: the chassis LM75 in
 * degrees C (hwmon, resolved by name; -273.15 when absent) and the
 * power supply's sensor as the raw 10-bit count (-1 when absent; the
 * conversion is unverified, so no degrees are published for it). */
double chassis_degc(void);
long supply_temp_raw(void);

/* The SoC die (the i.MX6 on-chip monitor, its thermal zone resolved by
 * type; -273.15 when absent) and the kernel's CPU-frequency cooling
 * state (0 = full speed, higher = throttled by the thermal governor;
 * -1 when there is no such cooling device). */
double soc_degc(void);
long soc_throttle_state(void);

#endif

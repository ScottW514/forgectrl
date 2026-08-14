/*
 * settings.h - persisted machine settings (see settings.c)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_SETTINGS_H
#define FORGECTRL_SETTINGS_H

#include <stddef.h>

/* Read a key from the shared config file (FORGECTRL_CONF, default
 * /data/forgefirm.conf). Returns 0 and fills val, or -1 if absent. */
int settings_get(const char *key, char *val, size_t len);

/* Set a key, preserving comments and unrelated keys; atomic replace.
 * An empty value removes the key. Returns 0 on success. */
int settings_set(const char *key, const char *val);

/* Apply count key/value pairs as ONE atomic replace, so no reader can
 * observe a half-applied multi-key update. Same per-key semantics as
 * settings_set. Mutations are serialized process-wide. */
int settings_set_many(const char *const *keys, const char *const *vals,
                      size_t count);

#endif

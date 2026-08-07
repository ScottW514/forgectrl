/*
 * settings.c - forgectrl: persisted machine settings
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Key/value store backed by a plain-text file on the persistent /data
 * partition. The file is shared machine configuration: other ForgeFIRM
 * components (the grblHAL-glowforge controller) read it with their own
 * parsers, so the format stays trivial - one "key = value" per line,
 * '#' comments, unknown keys preserved on rewrite. Writes go through a
 * temp file + rename so a concurrent reader never sees a partial file.
 */
#define _GNU_SOURCE
#include "settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SETTINGS_PATH_DEF "/data/forgefirm.conf"
#define LINE_MAX_LEN      256
#define FILE_MAX_LINES    64

static const char *settings_path(void)
{
    const char *v = getenv("FORGECTRL_CONF");
    return (v && *v) ? v : SETTINGS_PATH_DEF;
}

/* Parse "key = value" into trimmed pointers inside line (modified in
 * place). Returns 0 on a key/value line, -1 for comments/blank/other. */
static int parse_line(char *line, char **key, char **val)
{
    char *p = line;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '#' || *p == '\0')
        return -1;
    char *eq = strchr(p, '=');
    if (!eq)
        return -1;
    char *k_end = eq;
    while (k_end > p && isspace((unsigned char)k_end[-1]))
        k_end--;
    *k_end = '\0';
    char *v = eq + 1;
    while (isspace((unsigned char)*v))
        v++;
    char *v_end = v + strlen(v);
    while (v_end > v && isspace((unsigned char)v_end[-1]))
        v_end--;
    *v_end = '\0';
    if (*p == '\0')
        return -1;
    *key = p;
    *val = v;
    return 0;
}

int settings_get(const char *key, char *val, size_t len)
{
    FILE *f = fopen(settings_path(), "r");
    if (!f)
        return -1;

    char line[LINE_MAX_LEN];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        char *k, *v;
        if (parse_line(line, &k, &v) == 0 && !strcmp(k, key)) {
            snprintf(val, len, "%s", v);
            found = 0;
            /* keep scanning: the last occurrence wins */
        }
    }
    fclose(f);
    return found;
}

int settings_set(const char *key, const char *val)
{
    const char *path = settings_path();

    /* Read existing lines so unknown keys and comments survive. */
    char *lines[FILE_MAX_LINES];
    int n = 0, replaced = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        char line[LINE_MAX_LEN];
        while (n < FILE_MAX_LINES && fgets(line, sizeof(line), f)) {
            char probe[LINE_MAX_LEN], *k, *v;
            snprintf(probe, sizeof(probe), "%s", line);
            if (parse_line(probe, &k, &v) == 0 && !strcmp(k, key)) {
                if (replaced)
                    continue;        /* collapse duplicate keys */
                snprintf(line, sizeof(line), "%s = %s\n", key, val);
                replaced = 1;
            }
            lines[n] = strdup(line);
            if (!lines[n])
                break;
            n++;
        }
        fclose(f);
    }

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "settings: cannot write %s\n", tmp);
        for (int i = 0; i < n; i++)
            free(lines[i]);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        fputs(lines[i], f);
        free(lines[i]);
    }
    if (!replaced)
        fprintf(f, "%s = %s\n", key, val);
    int bad = ferror(f);
    if (fclose(f) != 0 || bad || rename(tmp, path) != 0) {
        fprintf(stderr, "settings: cannot update %s\n", path);
        remove(tmp);
        return -1;
    }
    return 0;
}

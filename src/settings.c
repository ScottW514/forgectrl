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
 * temp file + rename so a concurrent reader never sees a partial file,
 * mutations are serialized by a process-wide lock so concurrent writers
 * cannot lose keys, and a multi-key update lands as one rename so no
 * reader can observe it half-applied.
 */
#define _GNU_SOURCE
#include "fflog.h"
#include "settings.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t settings_mu = PTHREAD_MUTEX_INITIALIZER;

#define SETTINGS_PATH_DEF "/data/forgefirm.conf"
#define LINE_MAX_LEN      512
#define FILE_MAX_LINES    256

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

int settings_set_many(const char *const *keys, const char *const *vals,
                      size_t count)
{
    const char *path = settings_path();
    int ret = -1;

    if (count == 0)
        return 0;

    int *applied = calloc(count, sizeof(*applied));
    if (!applied)
        return -1;

    pthread_mutex_lock(&settings_mu);

    /* Read existing lines so unknown keys and comments survive. */
    char *lines[FILE_MAX_LINES];
    int n = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        char line[LINE_MAX_LEN];
        while (n < FILE_MAX_LINES && fgets(line, sizeof(line), f)) {
            char probe[LINE_MAX_LEN], *k, *v;
            snprintf(probe, sizeof(probe), "%s", line);
            if (parse_line(probe, &k, &v) == 0) {
                size_t m;
                for (m = 0; m < count; m++)
                    if (!strcmp(k, keys[m]))
                        break;
                if (m < count) {
                    /* Empty value = remove the key; duplicates collapse
                     * onto the first occurrence. */
                    if (vals[m][0] == '\0' || applied[m])
                        continue;
                    snprintf(line, sizeof(line), "%s = %s\n",
                             keys[m], vals[m]);
                    applied[m] = 1;
                }
            }
            lines[n] = strdup(line);
            if (!lines[n])
                break;
            n++;
        }
        /* NEVER rewrite a file that could not be read in full: silently
         * dropping the tail would eat unrelated keys (controller mode,
         * cooling tunables, cloud credentials). */
        if (n == FILE_MAX_LINES && fgetc(f) != EOF) {
            fflog(LOG_ERR, "settings: %s exceeds %d lines; refusing to "
                           "rewrite", path, FILE_MAX_LINES);
            fclose(f);
            for (int i = 0; i < n; i++)
                free(lines[i]);
            goto out;
        }
        fclose(f);
    } else {
        size_t m;
        for (m = 0; m < count; m++)
            if (vals[m][0] != '\0')
                break;
        if (m == count) {
            ret = 0;                 /* no file, nothing to remove */
            goto out;
        }
    }

    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    /* The file carries the cloud password: owner-only from creation. */
    int tfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    f = tfd >= 0 ? fdopen(tfd, "w") : NULL;
    if (!f) {
        if (tfd >= 0)
            close(tfd);
        fflog(LOG_ERR, "settings: cannot write %s", tmp);
        for (int i = 0; i < n; i++)
            free(lines[i]);
        goto out;
    }
    (void)fchmod(tfd, 0600);
    for (int i = 0; i < n; i++) {
        fputs(lines[i], f);
        free(lines[i]);
    }
    for (size_t m = 0; m < count; m++)
        if (!applied[m] && vals[m][0] != '\0')
            fprintf(f, "%s = %s\n", keys[m], vals[m]);
    /* fsync BEFORE the rename: under ext4 delayed allocation a power cut
     * just after the rename can otherwise leave an empty file - losing
     * the controller mode, cooling tunables, and cloud credentials at
     * once. */
    int bad = ferror(f);
    if (!bad && (fflush(f) != 0 || fsync(fileno(f)) != 0))
        bad = 1;
    if (fclose(f) != 0 || bad || rename(tmp, path) != 0) {
        fflog(LOG_ERR, "settings: cannot update %s", path);
        remove(tmp);
        goto out;
    }
    /* Best effort: persist the rename itself. */
    {
        char dir[300];
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = '\0';
            int dfd = open(dir, O_RDONLY | O_DIRECTORY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
    }
    ret = 0;

out:
    pthread_mutex_unlock(&settings_mu);
    free(applied);
    return ret;
}

int settings_set(const char *key, const char *val)
{
    const char *const keys[] = { key };
    const char *const vals[] = { val };
    return settings_set_many(keys, vals, 1);
}

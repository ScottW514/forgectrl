/*
 * logs.c - forgectrl: the ForgeFIRM logging tree
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Every ForgeFIRM process emits to syslog; rsyslog is the only file
 * writer and routes each program to its own directory under
 * /data/log/forgefirm. This module owns the machine-side pieces:
 *
 *  - render: turns the log_<logger>_disk / _remote settings and the
 *    remote-target settings into rsyslog rules
 *    (/data/forgefirm/rsyslog-forgefirm.conf, included by
 *    /etc/rsyslog.conf) plus a tmpfs record of the levels in force.
 *    Runs at boot before rsyslog starts, so settings apply on reboot;
 *    the panel shows configured vs. effective and asks for a reboot.
 *  - list / tail: what the panel's Logs tab reads.
 *  - export: a tar.gz of the whole tree plus a system snapshot, with
 *    an optional sanitizing pass (sanitize.c) for public issue reports.
 *
 * Emit-side rule: a process emits at the more verbose of its two
 * levels; rsyslog filters per destination. The kernel is the exception
 * (printk decides what is emitted); its levels only filter.
 */
#define _GNU_SOURCE
#include "logs.h"
#include "auth.h"
#include "fflog.h"
#include "sanitize.h"
#include "settings.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

const char *const logs_names[] = {
    "forgectrl", "grblhal", "gfcloud", "gfhome", "kernel", "system",
};
const size_t logs_count = sizeof(logs_names) / sizeof(*logs_names);

static const char *const level_names[] = {
    "off", "error", "warning", "notice", "info", "debug",
};
#define N_LEVELS (sizeof(level_names) / sizeof(*level_names))

#define TAIL_CAP       (64 * 1024)   /* bytes per tail response */
#define STAGE_DIR      "/data/forgefirm/tmp"
#define BUNDLE_TOP     "forgefirm-logs"
#define EXPORT_MAX_FILE (64L * 1024 * 1024)

/* ------------------------------------------------------- validators */

int logs_valid_level(const char *v)
{
    for (size_t i = 0; i < N_LEVELS; i++)
        if (!strcmp(v, level_names[i]))
            return 1;
    return 0;
}

int logs_valid_server(const char *v)
{
    size_t n = strlen(v);
    if (n < 1 || n > 128)
        return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)v[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':'))
            return 0;
    }
    return 1;
}

int logs_valid_port(const char *v)
{
    char *end;
    if (strlen(v) > 5)
        return 0;
    long p = strtol(v, &end, 10);
    return end != v && *end == '\0' && p >= 1 && p <= 65535;
}

int logs_valid_proto(const char *v)
{
    return !strcmp(v, "udp") || !strcmp(v, "tcp");
}

/* ---------------------------------------------------------- helpers */

static void get_or(const char *key, char *buf, size_t len, const char *dflt,
                   int (*valid)(const char *))
{
    if (settings_get(key, buf, len) != 0 || !buf[0] || !valid(buf))
        snprintf(buf, len, "%s", dflt);
}

/* Configured levels for a logger: disk defaults to info, remote to off. */
static void logger_levels(const char *name, char *disk, size_t dlen,
                          char *remote, size_t rlen)
{
    char key[64];
    snprintf(key, sizeof(key), "log_%s_disk", name);
    get_or(key, disk, dlen, "info", logs_valid_level);
    snprintf(key, sizeof(key), "log_%s_remote", name);
    get_or(key, remote, rlen, "off", logs_valid_level);
}

static void remote_target(char *server, size_t slen, char *port,
                          size_t plen, char *proto, size_t prlen)
{
    get_or("syslog_server", server, slen, "", logs_valid_server);
    get_or("syslog_port", port, plen, "514", logs_valid_port);
    get_or("syslog_proto", proto, prlen, "udp", logs_valid_proto);
}

/* Syslog severity for a level name; -1 for off. */
static int severity(const char *level)
{
    int l = -1;
    (void)fflog_parse_level(level, &l);
    return l;
}

/* Growing text buffer for the JSON replies. */
struct buf {
    char *p;
    size_t len, cap;
};

static void bput(struct buf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        size_t room = b->cap - b->len;
        int n = vsnprintf(b->p ? b->p + b->len : NULL, b->p ? room : 0,
                          fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (b->p && (size_t)n < room) {
            b->len += (size_t)n;
            return;
        }
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + (size_t)n + 1)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np)
            return;
        b->p = np;
        b->cap = nc;
    }
}

/* Append s as a JSON string body (no quotes), escaping as needed. */
static void bjson(struct buf *b, const char *s, size_t n)
{
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[8];
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n"; break;
        case '\r': esc = "\\r"; break;
        case '\t': esc = "\\t"; break;
        default:
            if (c < 0x20) {
                snprintf(u, sizeof(u), "\\u%04x", c);
                esc = u;
            }
        }
        if (!esc) {
            run++;
            continue;
        }
        if (run)
            bput(b, "%.*s", (int)run, s + i - run);
        run = 0;
        bput(b, "%s", esc);
    }
    if (run)
        bput(b, "%.*s", (int)run, s + n - run);
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* ----------------------------------------------------------- render */

static void render_logger(FILE *f, const char *name, const char *cond,
                          int remote_on)
{
    char disk[16], remote[16];
    logger_levels(name, disk, sizeof(disk), remote, sizeof(remote));
    int sd = severity(disk), sr = severity(remote);

    fprintf(f, "\n# %s: disk=%s remote=%s\n", name, disk, remote);
    const char *ind = cond ? "    " : "";
    if (cond)
        fprintf(f, "if %s then {\n", cond);
    else
        fprintf(f, "# everything else\n");
    if (sd >= 0)
        fprintf(f,
                "%sif $syslogseverity <= %d then action(type=\"omfile\""
                " file=\"%s/%s/%s.log\" template=\"ff_line\""
                " asyncWriting=\"on\" flushOnTXEnd=\"off\""
                " flushInterval=\"1\" ioBufferSize=\"64k\""
                " fileCreateMode=\"0640\" dirCreateMode=\"0755\""
                " createDirs=\"on\")\n",
                ind, sd, LOGS_ROOT, name, name);
    if (sr >= 0 && remote_on)
        fprintf(f, "%sif $syslogseverity <= %d then call ff_remote\n",
                ind, sr);
    fprintf(f, "%sstop\n%s", ind, cond ? "}\n" : "");
}

int logs_render(char *err, size_t errlen)
{
    if (mkdir_p(LOGS_ROOT, 0755) != 0) {
        snprintf(err, errlen, "cannot create %s: %s", LOGS_ROOT,
                 strerror(errno));
        return -1;
    }
    for (size_t i = 0; i < logs_count; i++) {
        char d[128];
        snprintf(d, sizeof(d), "%s/%s", LOGS_ROOT, logs_names[i]);
        (void)mkdir(d, 0755);
    }
    (void)mkdir_p(STAGE_DIR, 0700);

    char server[132], port[8], proto[8];
    remote_target(server, sizeof(server), port, sizeof(port), proto,
                  sizeof(proto));
    int remote_on = server[0] != '\0';

    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", LOGS_RSYSLOG);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errlen, "cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    fprintf(f,
        "# ForgeFIRM logging rules - GENERATED by forgectrl from\n"
        "# /data/forgefirm.conf at boot. Do not edit: change the levels\n"
        "# in the control panel (Logs tab) or in /data/forgefirm.conf and\n"
        "# reboot. Included by /etc/rsyslog.conf, which defines the\n"
        "# ff_line template and loads the input modules.\n");
    if (remote_on) {
        fprintf(f,
            "\n# remote target (RFC 5424 over %s); a per-action queue that\n"
            "# discards when full, so an unreachable server can never stall\n"
            "# the disk writers\n"
            "ruleset(name=\"ff_remote\") {\n"
            "    action(type=\"omfwd\" target=\"%s\" port=\"%s\""
            " protocol=\"%s\" template=\"RSYSLOG_SyslogProtocol23Format\""
            " queue.type=\"LinkedList\" queue.size=\"2000\""
            " queue.timeoutEnqueue=\"0\" action.resumeRetryCount=\"-1\""
            " action.resumeInterval=\"30\")\n"
            "}\n", proto, server, port, proto);
    } else {
        fprintf(f, "\n# no remote syslog server configured\n");
    }
    render_logger(f, "forgectrl", "$programname == \"forgectrl\"", remote_on);
    render_logger(f, "grblhal",   "$programname == \"grblhal\"",   remote_on);
    render_logger(f, "gfcloud",   "$programname == \"gfcloud\"",   remote_on);
    render_logger(f, "gfhome",    "$programname == \"gfhome\"",    remote_on);
    render_logger(f, "kernel",    "$syslogfacility-text == \"kern\"", remote_on);
    render_logger(f, "system",    NULL, remote_on);
    if (fclose(f) != 0 || rename(tmp, LOGS_RSYSLOG) != 0) {
        snprintf(err, errlen, "cannot install %s: %s", LOGS_RSYSLOG,
                 strerror(errno));
        unlink(tmp);
        return -1;
    }

    /* The levels now in force, for the panel's configured-vs-effective
     * view (tmpfs: gone at reboot, rewritten by the next render). */
    snprintf(tmp, sizeof(tmp), "%s.tmp", LOGS_EFFECTIVE);
    f = fopen(tmp, "w");
    if (f) {
        for (size_t i = 0; i < logs_count; i++) {
            char disk[16], remote[16];
            logger_levels(logs_names[i], disk, sizeof(disk), remote,
                          sizeof(remote));
            fprintf(f, "log_%s_disk=%s\nlog_%s_remote=%s\n",
                    logs_names[i], disk, logs_names[i], remote);
        }
        fprintf(f, "syslog_server=%s\nsyslog_port=%s\nsyslog_proto=%s\n",
                server, port, proto);
        fclose(f);
        (void)rename(tmp, LOGS_EFFECTIVE);
    }
    return 0;
}

/* ------------------------------------------------------------- list */

/* Read one key from LOGS_EFFECTIVE; returns 0 and fills val, -1 if
 * absent (no render since boot, or an image without one). */
static int effective_get(const char *key, char *val, size_t len)
{
    FILE *f = fopen(LOGS_EFFECTIVE, "r");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            snprintf(val, len, "%s", line + kl + 1);
            val[strcspn(val, "\r\n")] = '\0';
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

static void dir_usage(const char *name, unsigned long long *bytes, int *files)
{
    char d[128];
    snprintf(d, sizeof(d), "%s/%s", LOGS_ROOT, name);
    *bytes = 0;
    *files = 0;
    DIR *dp = opendir(d);
    if (!dp)
        return;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char p[512];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            *bytes += (unsigned long long)st.st_size;
            (*files)++;
        }
    }
    closedir(dp);
}

char *logs_list_json(void)
{
    struct buf b = { NULL, 0, 0 };
    int pending = 0;
    int have_eff = access(LOGS_EFFECTIVE, R_OK) == 0;

    bput(&b, "{\"root\":\"%s\",\"levels\":[", LOGS_ROOT);
    for (size_t i = 0; i < N_LEVELS; i++)
        bput(&b, "%s\"%s\"", i ? "," : "", level_names[i]);
    bput(&b, "],\"loggers\":[");
    for (size_t i = 0; i < logs_count; i++) {
        char disk[16], remote[16], ed[16] = "", er[16] = "", key[64];
        logger_levels(logs_names[i], disk, sizeof(disk), remote,
                      sizeof(remote));
        snprintf(key, sizeof(key), "log_%s_disk", logs_names[i]);
        (void)effective_get(key, ed, sizeof(ed));
        snprintf(key, sizeof(key), "log_%s_remote", logs_names[i]);
        (void)effective_get(key, er, sizeof(er));
        if (have_eff && (strcmp(disk, ed) || strcmp(remote, er)))
            pending = 1;
        unsigned long long bytes;
        int files;
        dir_usage(logs_names[i], &bytes, &files);
        bput(&b, "%s{\"name\":\"%s\",\"disk\":\"%s\",\"remote\":\"%s\","
                 "\"effective_disk\":\"%s\",\"effective_remote\":\"%s\","
                 "\"bytes\":%llu,\"files\":%d}",
             i ? "," : "", logs_names[i], disk, remote, ed, er, bytes, files);
    }
    char server[132], port[8], proto[8], es[132] = "", ep[8] = "", epr[8] = "";
    remote_target(server, sizeof(server), port, sizeof(port), proto,
                  sizeof(proto));
    (void)effective_get("syslog_server", es, sizeof(es));
    (void)effective_get("syslog_port", ep, sizeof(ep));
    (void)effective_get("syslog_proto", epr, sizeof(epr));
    if (have_eff && (strcmp(server, es) || strcmp(port, ep) ||
                     strcmp(proto, epr)))
        pending = 1;
    bput(&b, "],\"syslog_server\":\"%s\",\"syslog_port\":\"%s\","
             "\"syslog_proto\":\"%s\",\"effective_syslog_server\":\"%s\","
             "\"effective_known\":%s,\"pending_reboot\":%s}",
         server, port, proto, es, have_eff ? "true" : "false",
         pending ? "true" : "false");
    return b.p;
}

/* ------------------------------------------------------------- tail */

static int known_logger(const char *name)
{
    for (size_t i = 0; i < logs_count; i++)
        if (!strcmp(name, logs_names[i]))
            return 1;
    return 0;
}

char *logs_tail_json(const char *name, long lines, long long from)
{
    if (!name || !known_logger(name))
        return NULL;
    if (lines < 1)
        lines = 200;
    if (lines > 2000)
        lines = 2000;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s/%s.log", LOGS_ROOT, name, name);
    struct buf b = { NULL, 0, 0 };
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        if (fd >= 0)
            close(fd);
        bput(&b, "{\"name\":\"%s\",\"size\":0,\"offset\":0,\"text\":\"\","
                 "\"truncated\":false,\"exists\":false}", name);
        return b.p;
    }
    long long size = (long long)st.st_size;
    int incremental = from >= 0 && from <= size;
    long long start = incremental ? from : 0;
    int truncated = 0;
    if (size - start > TAIL_CAP) {
        start = size - TAIL_CAP;
        truncated = 1;
    }
    size_t want = (size_t)(size - start);
    char *data = malloc(want + 1);
    if (!data) {
        close(fd);
        return NULL;
    }
    size_t got = 0;
    while (got < want) {
        ssize_t r = pread(fd, data + got, want - got, start + (off_t)got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    data[got] = '\0';

    const char *text = data;
    size_t tlen = got;
    if (truncated) {
        /* drop the partial first line */
        const char *nl = memchr(text, '\n', tlen);
        if (nl) {
            tlen -= (size_t)(nl + 1 - text);
            text = nl + 1;
        }
    }
    if (!incremental) {
        /* keep the last `lines` lines */
        long n = 0;
        const char *p = text + tlen;
        while (p > text) {
            p--;
            if (*p == '\n' && p != text + tlen - 1) {
                if (++n >= lines) {
                    p++;
                    break;
                }
            }
        }
        tlen -= (size_t)(p - text);
        text = p;
    }
    bput(&b, "{\"name\":\"%s\",\"size\":%lld,\"offset\":%lld,\"text\":\"",
         name, size, size);
    bjson(&b, text, tlen);
    bput(&b, "\",\"truncated\":%s,\"exists\":true}",
         truncated ? "true" : "false");
    free(data);
    return b.p;
}

/* ----------------------------------------------------------- export */

#define PSTORE_DIR "/sys/fs/pstore"   /* ramoops records from the last panics */

struct logs_export {
    char base[128];
    FILE *pipe;
};

static pthread_mutex_t export_mu = PTHREAD_MUTEX_INITIALIZER;
static int export_busy = 0;

static int rm_cb(const char *path, const struct stat *st, int flag,
                 struct FTW *ftw)
{
    (void)st;
    (void)ftw;
    return (flag == FTW_DP || flag == FTW_D) ? rmdir(path) : unlink(path);
}

static void rm_rf(const char *path)
{
    (void)nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* Any staging left by an interrupted export. */
static void clean_stage(void)
{
    DIR *dp = opendir(STAGE_DIR);
    if (!dp)
        return;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (!strncmp(e->d_name, "export.", 7)) {
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", STAGE_DIR, e->d_name);
            rm_rf(p);
        }
    }
    closedir(dp);
}

/* Register the machine's identifying values with the sanitizer: exact
 * matching catches what patterns cannot (a hostname, a passphrase). */
static void add_wpa_known(sanitizer_t *san, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p))
            p++;
        const char *tag = NULL;
        if (!strncmp(p, "ssid=", 5)) { tag = "SSID"; p += 5; }
        else if (!strncmp(p, "psk=", 4)) { tag = "PSK"; p += 4; }
        else if (!strncmp(p, "identity=", 9)) { tag = "WIFI_IDENTITY"; p += 9; }
        else if (!strncmp(p, "password=", 9)) { tag = "WIFI_PASSWORD"; p += 9; }
        else if (!strncmp(p, "bssid=", 6)) { tag = "BSSID"; p += 6; }
        if (!tag)
            continue;
        p[strcspn(p, "\r\n")] = '\0';
        size_t n = strlen(p);
        if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
            p[n - 1] = '\0';
            p++;
        }
        if (*p)
            san_add_known(san, tag, p);
    }
    fclose(f);
}

static void load_known(sanitizer_t *san)
{
    char v[132];
    unsigned long ser = fuse_serial();
    if (ser) {
        snprintf(v, sizeof(v), "%lu", ser);
        san_add_known(san, "SERIAL", v);
    }
    char mid[16];
    machine_id(mid, sizeof(mid));
    if (mid[0])
        san_add_known(san, "HOSTNAME", mid);
    char pw[65];
    if (fuse_password(pw, sizeof(pw)) == 0 && pw[0])
        san_add_known(san, "GF_PASSWORD", pw);
    if (settings_get("gf_serial", v, sizeof(v)) == 0)
        san_add_known(san, "SERIAL", v);
    if (settings_get("gf_password", v, sizeof(v)) == 0)
        san_add_known(san, "GF_PASSWORD", v);
    if (settings_get("syslog_server", v, sizeof(v)) == 0)
        san_add_known(san, "SYSLOG_SERVER", v);
    const char *tok = auth_token();
    if (tok && *tok)
        san_add_known(san, "PANEL_TOKEN", tok);
    char hn[128];
    if (gethostname(hn, sizeof(hn)) == 0) {
        hn[sizeof(hn) - 1] = '\0';
        if (strcmp(hn, "glowforge") && strcmp(hn, "localhost"))
            san_add_known(san, "HOSTNAME", hn);
    }
    add_wpa_known(san, "/data/etc/wpa_supplicant.conf");
    add_wpa_known(san, "/etc/wpa_supplicant.conf");
}

/* Copy a text stream line by line into dst, sanitizing if san. */
static int stage_stream(sanitizer_t *san, FILE *in, const char *dst)
{
    FILE *out = fopen(dst, "w");
    if (!out)
        return -1;
    char line[16384];
    while (fgets(line, sizeof(line), in)) {
        if (san) {
            char *o = san_line(san, line);
            fputs(o ? o : "<line dropped: out of memory>\n", out);
            free(o);
        } else {
            fputs(line, out);
        }
    }
    return fclose(out) == 0 ? 0 : -1;
}

static int stage_file(sanitizer_t *san, const char *src, const char *dst)
{
    FILE *in = fopen(src, "r");
    if (!in)
        return -1;
    int rc = stage_stream(san, in, dst);
    fclose(in);
    return rc;
}

static int stage_cmd(sanitizer_t *san, const char *cmd, const char *dst)
{
    FILE *in = popen(cmd, "r");
    if (!in)
        return -1;
    int rc = stage_stream(san, in, dst);
    pclose(in);
    return rc;
}

/* A rotated (gzip) log: decompress, sanitize, recompress. */
static int stage_gz(sanitizer_t *san, const char *src, const char *dst)
{
    gzFile in = gzopen(src, "rb");
    if (!in)
        return -1;
    gzFile out = gzopen(dst, "wb6");
    if (!out) {
        gzclose(in);
        return -1;
    }
    char line[16384];
    while (gzgets(in, line, sizeof(line))) {
        if (san) {
            char *o = san_line(san, line);
            gzputs(out, o ? o : "<line dropped: out of memory>\n");
            free(o);
        } else {
            gzputs(out, line);
        }
    }
    gzclose(in);
    return gzclose(out) == Z_OK ? 0 : -1;
}

static int has_suffix(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && !strcmp(s + n - m, suf);
}

logs_export_t *logs_export_begin(int sanitize, void (*settings_cb)(FILE *),
                                 char *err, size_t errlen)
{
    pthread_mutex_lock(&export_mu);
    if (export_busy) {
        pthread_mutex_unlock(&export_mu);
        snprintf(err, errlen, "busy");
        return NULL;
    }
    export_busy = 1;
    pthread_mutex_unlock(&export_mu);

    logs_export_t *e = calloc(1, sizeof(*e));
    sanitizer_t *san = NULL;
    if (!e)
        goto fail_oom;
    (void)mkdir_p(STAGE_DIR, 0700);
    clean_stage();
    snprintf(e->base, sizeof(e->base), "%s/export.XXXXXX", STAGE_DIR);
    if (!mkdtemp(e->base)) {
        snprintf(err, errlen, "cannot create staging: %s", strerror(errno));
        goto fail;
    }
    if (sanitize) {
        san = san_new();
        if (!san)
            goto fail_oom;
        load_known(san);
    }

    char top[192], d[256], src[640], dst[640];
    snprintf(top, sizeof(top), "%s/%s", e->base, BUNDLE_TOP);
    snprintf(d, sizeof(d), "%s/logs", top);
    if (mkdir_p(d, 0700) != 0)
        goto fail_stage;
    snprintf(d, sizeof(d), "%s/system", top);
    if (mkdir(d, 0700) != 0)
        goto fail_stage;

    /* the log tree */
    for (size_t i = 0; i < logs_count; i++) {
        snprintf(d, sizeof(d), "%s/logs/%s", top, logs_names[i]);
        (void)mkdir(d, 0700);
        char sd[160];
        snprintf(sd, sizeof(sd), "%s/%s", LOGS_ROOT, logs_names[i]);
        DIR *dp = opendir(sd);
        if (!dp)
            continue;
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            snprintf(src, sizeof(src), "%s/%s", sd, de->d_name);
            snprintf(dst, sizeof(dst), "%s/%s", d, de->d_name);
            struct stat st;
            if (stat(src, &st) != 0 || !S_ISREG(st.st_mode) ||
                st.st_size > EXPORT_MAX_FILE)
                continue;
            if (has_suffix(de->d_name, ".gz"))
                (void)stage_gz(san, src, dst);
            else
                (void)stage_file(san, src, dst);
        }
        closedir(dp);
    }

    /* system snapshot */
    snprintf(dst, sizeof(dst), "%s/system/version.txt", top);
    (void)stage_file(san, "/etc/forgefirm-version", dst);
    snprintf(dst, sizeof(dst), "%s/system/cmdline.txt", top);
    (void)stage_file(san, "/proc/cmdline", dst);
    snprintf(dst, sizeof(dst), "%s/system/uptime.txt", top);
    (void)stage_cmd(san, "date; cat /proc/uptime", dst);
    snprintf(dst, sizeof(dst), "%s/system/meminfo.txt", top);
    (void)stage_file(san, "/proc/meminfo", dst);
    snprintf(dst, sizeof(dst), "%s/system/dmesg.txt", top);
    (void)stage_cmd(san, "dmesg 2>&1", dst);
    snprintf(dst, sizeof(dst), "%s/system/df.txt", top);
    (void)stage_cmd(san, "df -h 2>&1", dst);
    snprintf(dst, sizeof(dst), "%s/system/ps.txt", top);
    (void)stage_cmd(san, "ps 2>&1", dst);
    snprintf(dst, sizeof(dst), "%s/system/rsyslog-forgefirm.conf", top);
    (void)stage_file(san, LOGS_RSYSLOG, dst);
    snprintf(dst, sizeof(dst), "%s/system/loglevels.txt", top);
    (void)stage_file(san, LOGS_EFFECTIVE, dst);

    /* crash records: what pstore (ramoops) carried across the last panic
     * reboots, if anything; the directory exists only when it holds some */
    DIR *pp = opendir(PSTORE_DIR);
    if (pp) {
        int made = 0;
        struct dirent *de;
        while ((de = readdir(pp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;
            snprintf(src, sizeof(src), "%s/%s", PSTORE_DIR, de->d_name);
            struct stat st;
            if (stat(src, &st) != 0 || !S_ISREG(st.st_mode) ||
                st.st_size > EXPORT_MAX_FILE)
                continue;
            if (!made) {
                snprintf(d, sizeof(d), "%s/system/pstore", top);
                (void)mkdir(d, 0700);
                made = 1;
            }
            snprintf(dst, sizeof(dst), "%s/system/pstore/%s", top, de->d_name);
            (void)stage_file(san, src, dst);
        }
        closedir(pp);
    }
    if (settings_cb) {
        FILE *t = tmpfile();
        if (t) {
            settings_cb(t);
            rewind(t);
            snprintf(dst, sizeof(dst), "%s/system/settings.txt", top);
            (void)stage_stream(san, t, dst);
            fclose(t);
        }
    }

    /* README, written last so it can carry the redaction report */
    snprintf(dst, sizeof(dst), "%s/README.txt", top);
    FILE *r = fopen(dst, "w");
    if (r) {
        char ts[64];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %z", &tm);
        fprintf(r, "ForgeFIRM log export\n"
                   "created: %s\n"
                   "sanitized: %s\n\n"
                   "Layout:\n"
                   "  logs/<logger>/   one directory per logger (forgectrl,"
                   " grblhal, gfcloud, gfhome,\n"
                   "                   kernel, system): the live .log and"
                   " rotated .N.gz files\n"
                   "  system/          firmware version, kernel ring buffer,"
                   " uptime, memory, disk,\n"
                   "                   processes, effective log levels,"
                   " settings (secrets masked)\n"
                   "  system/pstore/   crash records the kernel kept across"
                   " its last panic reboots\n"
                   "                   (ramoops), present only when there"
                   " are any\n\n",
                ts, sanitize ? "yes" : "NO - contains the machine identity,"
                                     " network addresses, and any credentials"
                                     " the logs carry");
        if (sanitize) {
            fprintf(r, "Redactions applied (placeholders keep the same"
                       " number for the same value\n"
                       "throughout this bundle, so hosts and addresses can"
                       " still be correlated):\n");
            if (san_total(san) == 0)
                fprintf(r, "  (none needed)\n");
            else
                san_report(san, r);
            fprintf(r, "\nThe sanitizer removes this machine's known"
                       " identifiers (serial, hostname, cloud\n"
                       "credentials, panel token, WiFi network) and common"
                       " patterns (network addresses,\n"
                       "e-mail addresses, bearer tokens, key=value secrets,"
                       " long hex/base64 blobs). It\n"
                       "cannot know everything a log line may carry - skim"
                       " the bundle before posting it\n"
                       "publicly.\n");
        }
        fclose(r);
    }
    san_free(san);
    san = NULL;

    char cmd[320];
    snprintf(cmd, sizeof(cmd), "cd '%s' && tar -cf - %s | gzip -c",
             e->base, BUNDLE_TOP);
    e->pipe = popen(cmd, "r");
    if (!e->pipe) {
        snprintf(err, errlen, "cannot start the archiver: %s",
                 strerror(errno));
        goto fail;
    }
    return e;

fail_stage:
    snprintf(err, errlen, "cannot stage the bundle: %s", strerror(errno));
    goto fail;
fail_oom:
    snprintf(err, errlen, "out of memory");
fail:
    san_free(san);
    if (e) {
        if (e->base[0])
            rm_rf(e->base);
        free(e);
    }
    pthread_mutex_lock(&export_mu);
    export_busy = 0;
    pthread_mutex_unlock(&export_mu);
    return NULL;
}

ssize_t logs_export_read(logs_export_t *e, char *buf, size_t max)
{
    if (!e || !e->pipe)
        return -1;
    size_t n = fread(buf, 1, max, e->pipe);
    if (n == 0)
        return ferror(e->pipe) ? -1 : 0;
    return (ssize_t)n;
}

void logs_export_end(logs_export_t *e)
{
    if (!e)
        return;
    if (e->pipe)
        pclose(e->pipe);
    rm_rf(e->base);
    free(e);
    pthread_mutex_lock(&export_mu);
    export_busy = 0;
    pthread_mutex_unlock(&export_mu);
}

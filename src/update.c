/*
 * update.c - forgectrl: firmware update manager
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Slot inventory (ffboot -l), boot-target selection, ForgeFIRM release
 * check / download / apply, dev-archive upload, and factory restore
 * from the /data archive. The A/B slot scheme and its invariants are
 * documented in the forgefirm repo (docs/UPDATE-SYSTEM.md).
 *
 * Long operations run on a single detached worker (diag.c's model):
 * one job at a time, status polled from the UI. Every flash write
 * requires an idle machine and no running diagnostic, takes the update
 * lock (flock on /data/forgefirm/update.lock - the convention shared
 * with the installer), verifies the archive signature before writing,
 * writes only a slot that is NOT the booted root, and re-verifies the
 * written filesystem afterward. Switching the boot target never
 * happens implicitly: it is its own explicit action, probe-gated by
 * ffboot itself.
 *
 * Release downloads resolve the version WITHOUT the GitHub API: the
 * fixed-name asset URL redirects to .../download/v<ver>/forgefirm.fw,
 * so a HEAD request's effective URL carries the version - no rate
 * limits, no JSON.
 */
#define _GNU_SOURCE
#include "update.h"
#include "diag.h"
#include "status.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FFBOOT      "/usr/sbin/ffboot"
#define DATA_DIR    "/data/forgefirm"
#define ARCHIVE_DIR DATA_DIR "/archive"
#define DL_FW       DATA_DIR "/download.fw"
#define UP_FW       DATA_DIR "/upload.fw"
#define LOCK_FILE   DATA_DIR "/update.lock"
#define KEY_RELEASE "/etc/forgefirm/keys/forgefirm-release.pub"
#define KEY_GF_DIR  "/etc/forgefirm/keys/gf"
#define LATEST_URL \
    "https://github.com/ScottW514/forgefirm/releases/latest/download/forgefirm.fw"
/* An upload larger than any plausible archive is cut off (slot is
 * 200 MiB; a .fw compresses well below that). */
#define UPLOAD_MAX  (256UL * 1024 * 1024)

/* ------------------------------------------------------------- state */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int    job_running;
static char   job_kind[24];
static char   job_phase[96];
static time_t job_started;
static char   job_result[768];      /* JSON object, or "" */
static char   progress_file[64];    /* file whose growth is progress */

static pthread_mutex_t up_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE  *up_fp;
static uint64_t up_bytes;
static int    up_error;

/* ----------------------------------------------------------- helpers */

static int reply_json(struct _u_response *res, unsigned status,
                      const char *body)
{
    ulfius_set_string_body_response(res, status, body);
    ulfius_add_header_to_response(res, "Content-Type", "application/json");
    return U_CALLBACK_CONTINUE;
}

static int reply_err(struct _u_response *res, unsigned status,
                     const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return reply_json(res, status, body);
}

/* Copy src into dst JSON-safely: printable chars minus quote/backslash. */
static void jsan(char *dst, size_t len, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src && src[i] && o + 1 < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= ' ' && c != '"' && c != '\\' && c < 0x7f)
            dst[o++] = (char)c;
        else if (c == '\n' && o && dst[o - 1] != ' ')
            dst[o++] = ' ';
    }
    while (o > 0 && dst[o - 1] == ' ')
        o--;
    dst[o] = '\0';
}

/* Run a command, capture combined output (sanitized), return exit code
 * (negative on popen failure). */
static int run_cmd(char *out, size_t outlen, const char *cmd)
{
    if (out && outlen)
        out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    char raw[2048];
    size_t n = fread(raw, 1, sizeof(raw) - 1, p);
    raw[n] = '\0';
    while (fgetc(p) != EOF)
        ;                               /* drain so the child can exit */
    int st = pclose(p);
    if (out && outlen)
        jsan(out, outlen, raw);
    if (st < 0)
        return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static const char *booted_root(void)
{
    static char root[32];
    if (!root[0]) {
        FILE *f = fopen("/proc/cmdline", "r");
        char line[512] = "";
        if (f) {
            if (!fgets(line, sizeof(line), f))
                line[0] = '\0';
            fclose(f);
        }
        char *p = strstr(line, "root=");
        if (p) {
            p += 5;
            size_t o = 0;
            while (*p && *p != ' ' && o + 1 < sizeof(root))
                root[o++] = *p++;
            root[o] = '\0';
        }
    }
    return root;
}

struct slot_target {
    const char *name;
    const char *dev;
    const char *ffboot_arg;         /* boot-select argument */
    const char *task;               /* fwup upgrade task, or NULL */
};
static const struct slot_target targets[] = {
    { "sd",     "/dev/mmcblk1p1", "-s",  NULL },
    { "a",      "/dev/mmcblk2p1", "-e1", "upgrade.a" },
    { "b",      "/dev/mmcblk2p2", "-e2", "upgrade.b" },
    { "legacy", "/dev/mmcblk2p4", "-e4", NULL },
};
#define N_TARGETS (sizeof(targets) / sizeof(*targets))

static const struct slot_target *find_target(const char *name)
{
    for (size_t i = 0; name && i < N_TARGETS; i++)
        if (!strcmp(targets[i].name, name))
            return &targets[i];
    return NULL;
}

/* A write target must be the factory 200 MiB slot geometry (409600
 * 512-byte sectors) - the same check the installer makes. This refuses
 * a repartitioned or absent slot before a raw image write, so it cannot
 * overflow the partition or land on an unexpected layout. */
static int slot_geometry_ok(const struct slot_target *t)
{
    const char *base = strrchr(t->dev, '/');
    base = base ? base + 1 : t->dev;
    char path[64], buf[24];
    snprintf(path, sizeof(path), "/sys/class/block/%s/size", base);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int ok = fgets(buf, sizeof(buf), f) && atol(buf) == 409600;
    fclose(f);
    return ok;
}

static const char *param(const struct _u_request *req, const char *key)
{
    const char *v = u_map_get(req->map_post_body, key);
    return v ? v : u_map_get(req->map_url, key);
}

/* fwup -m meta-version of an archive (empty string if unreadable). */
static void fw_meta_version(const char *file, char *out, size_t len)
{
    char cmd[256], raw[192];
    out[0] = '\0';
    snprintf(cmd, sizeof(cmd),
             "fwup -m -i %s 2>/dev/null | sed -n 's/^meta-version=\"\\{0,1\\}\\([^\"]*\\).*/\\1/p'",
             file);
    if (run_cmd(raw, sizeof(raw), cmd) == 0)
        jsan(out, len, raw);
}

/* Signature classification: 2 = ForgeFIRM release key, 1 = a Glowforge
 * factory key, 0 = valid fwup archive but neither key, -1 = not a
 * usable fwup archive. */
static int fw_classify(const char *file)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fwup -V -i %s -p %s >/dev/null 2>&1",
             file, KEY_RELEASE);
    if (run_cmd(NULL, 0, cmd) == 0)
        return 2;
    DIR *d = opendir(KEY_GF_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(cmd, sizeof(cmd),
                     "fwup -V -i %s -p " KEY_GF_DIR "/%s >/dev/null 2>&1",
                     file, e->d_name);
            if (run_cmd(NULL, 0, cmd) == 0) {
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }
    snprintf(cmd, sizeof(cmd), "fwup -l -i %s >/dev/null 2>&1", file);
    return run_cmd(NULL, 0, cmd) == 0 ? 0 : -1;
}

/* Semantic version an archive was recorded with, from the manifest's
 * "ver=<semantic>" field. Empty if the archive is not in the manifest,
 * has no ver= (older installer), or ver=unknown. */
static void archive_manifest_version(const char *file, char *out, size_t len)
{
    out[0] = '\0';
    FILE *f = fopen(ARCHIVE_DIR "/manifest", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, file))
            continue;
        char *v = strstr(line, "ver=");
        if (v) {
            v += 4;
            char raw[48];
            size_t o = 0;
            while (v[o] && v[o] != ' ' && v[o] != '\n' && o + 1 < sizeof(raw)) {
                raw[o] = v[o];
                o++;
            }
            raw[o] = '\0';
            if (strcmp(raw, "unknown") != 0)
                jsan(out, len, raw);
        }
        break;
    }
    fclose(f);
}

/* ------------------------------------------------------ job machinery */

int update_job_running(void)
{
    pthread_mutex_lock(&mu);
    int r = job_running;
    pthread_mutex_unlock(&mu);
    return r;
}

static void job_set_phase(const char *phase)
{
    pthread_mutex_lock(&mu);
    snprintf(job_phase, sizeof(job_phase), "%s", phase);
    pthread_mutex_unlock(&mu);
}

static void job_finish(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&mu);
    vsnprintf(job_result, sizeof(job_result), fmt, ap);
    job_running = 0;
    job_phase[0] = '\0';
    progress_file[0] = '\0';
    pthread_mutex_unlock(&mu);
    va_end(ap);
}

/* Start a job: 0 ok, -1 busy, -2 not idle, -3 diagnostic running. */
static int job_start(const char *kind, void *(*worker)(void *), void *arg)
{
    if (diag_running())
        return -3;
    if (!machine_is_idle())
        return -2;
    pthread_mutex_lock(&mu);
    if (job_running) {
        pthread_mutex_unlock(&mu);
        return -1;
    }
    job_running = 1;
    snprintf(job_kind, sizeof(job_kind), "%s", kind);
    job_phase[0] = '\0';
    job_result[0] = '\0';
    progress_file[0] = '\0';
    job_started = time(NULL);
    pthread_mutex_unlock(&mu);

    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&th, &at, worker, arg);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        pthread_mutex_lock(&mu);
        job_running = 0;
        pthread_mutex_unlock(&mu);
        return -1;
    }
    return 0;
}

/* Reply for a job_start return code. On 0 the worker owns `arg`; on
 * any error the CALLER still owns it and must free. */
static int job_start_reply(struct _u_response *res, int rc)
{
    switch (rc) {
    case 0:
        return reply_json(res, 202, "{\"started\":true}");
    case -1:
        return reply_err(res, 409, "an update job is already running");
    case -2:
        return reply_err(res, 409, "machine is not idle");
    default:
        return reply_err(res, 409, "a diagnostic is running");
    }
}

/* The update lock, shared by convention with the installer. */
static int lock_fd = -1;

static int take_lock(void)
{
    mkdir(DATA_DIR, 0755);
    lock_fd = open(LOCK_FILE, O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0)
        return -1;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        close(lock_fd);
        lock_fd = -1;
        return -1;
    }
    return 0;
}

static void drop_lock(void)
{
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        lock_fd = -1;
    }
}

/* --------------------------------------------------------- inventory */

struct slot_info {
    char present[8], state[16], type[16], version[64], kernel[8];
    int booted, next;
};

int cb_slots(const struct _u_request *req, struct _u_response *res,
             void *user_data)
{
    (void)req;
    (void)user_data;

    char raw[4096];
    FILE *p = popen(FFBOOT " -l 2>/dev/null", "r");
    if (!p)
        return reply_err(res, 500, "cannot run ffboot");
    size_t n = fread(raw, 1, sizeof(raw) - 1, p);
    raw[n] = '\0';
    pclose(p);

    char env_json[512] = "";
    size_t eo = 0;
    struct slot_info si[N_TARGETS];
    memset(si, 0, sizeof(si));

    char *save = NULL;
    for (char *ln = strtok_r(raw, "\n", &save); ln;
         ln = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(ln, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char val[96];
        jsan(val, sizeof(val), eq + 1);
        if (!strncmp(ln, "env.", 4)) {
            eo += (size_t)snprintf(env_json + eo, sizeof(env_json) - eo,
                                   "%s\"%s\":\"%s\"", eo ? "," : "",
                                   ln + 4, val);
            continue;
        }
        if (strncmp(ln, "slot.", 5))
            continue;
        char *dot = strchr(ln + 5, '.');
        if (!dot)
            continue;
        *dot = '\0';
        const char *field = dot + 1;
        for (size_t t = 0; t < N_TARGETS; t++) {
            if (strcmp(ln + 5, targets[t].name))
                continue;
            if (!strcmp(field, "present"))
                snprintf(si[t].present, sizeof(si[t].present), "%s", val);
            else if (!strcmp(field, "state"))
                snprintf(si[t].state, sizeof(si[t].state), "%s", val);
            else if (!strcmp(field, "type"))
                snprintf(si[t].type, sizeof(si[t].type), "%s", val);
            else if (!strcmp(field, "version"))
                snprintf(si[t].version, sizeof(si[t].version), "%s", val);
            else if (!strcmp(field, "kernel"))
                snprintf(si[t].kernel, sizeof(si[t].kernel), "%s", val);
            else if (!strcmp(field, "booted"))
                si[t].booted = 1;
            else if (!strcmp(field, "next"))
                si[t].next = 1;
        }
    }

    char body[8192];
    size_t off = 0;
    off += (size_t)snprintf(body + off, sizeof(body) - off,
                            "{\"booted\":\"%s\",\"env\":{%s},\"slots\":{",
                            booted_root(), env_json);
    /* Always show the two firmware slots (a/b). Only surface sd and the
     * legacy partition when they actually exist: on a normal install the
     * legacy partition was reclaimed at first boot, so a "legacy: not
     * present" line is just noise - it should appear only if for some
     * reason it could not be removed. */
    int emitted = 0;
    for (size_t t = 0; t < N_TARGETS; t++) {
        int present = si[t].present[0] && !strcmp(si[t].present, "yes");
        int always = !strcmp(targets[t].name, "a") ||
                     !strcmp(targets[t].name, "b");
        if (!always && !present)
            continue;
        off += (size_t)snprintf(body + off, sizeof(body) - off,
            "%s\"%s\":{\"device\":\"%s\",\"present\":\"%s\","
            "\"state\":\"%s\",\"type\":\"%s\",\"version\":\"%s\","
            "\"kernel\":\"%s\",\"booted\":%s,\"next\":%s}",
            emitted ? "," : "", targets[t].name, targets[t].dev,
            si[t].present[0] ? si[t].present : "no",
            si[t].state, si[t].type, si[t].version, si[t].kernel,
            si[t].booted ? "true" : "false",
            si[t].next ? "true" : "false");
        emitted = 1;
    }
    off += (size_t)snprintf(body + off, sizeof(body) - off,
                            "},\"archives\":[");

    /* Only factory-rootfs archives are user-restorable; recovery-boot
     * blobs are a Phase-5 concern and would only confuse the restore
     * list, so they are omitted here. The semantic version comes from
     * the manifest's ver= field (written by the installer); without it
     * (older archives) the display falls back to the build date parsed
     * from the filename. */
    DIR *d = opendir(ARCHIVE_DIR);
    int first = 1;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && off + 320 < sizeof(body)) {
            if (strncmp(e->d_name, "factory-rootfs-", 15))
                continue;
            char path[512];
            struct stat st;
            snprintf(path, sizeof(path), ARCHIVE_DIR "/%s", e->d_name);
            if (stat(path, &st) != 0)
                continue;
            char name[160], ver[48] = "", date[24] = "";
            jsan(name, sizeof(name), e->d_name);
            archive_manifest_version(e->d_name, ver, sizeof(ver));
            /* filename is factory-rootfs-<YYYYMMDDHHMMSS>.img.gz */
            const char *ds = e->d_name + 15;
            if (strlen(ds) >= 8)
                snprintf(date, sizeof(date), "%.4s-%.2s-%.2s",
                         ds, ds + 4, ds + 6);
            off += (size_t)snprintf(body + off, sizeof(body) - off,
                "%s{\"file\":\"%s\",\"bytes\":%ld,\"version\":\"%s\","
                "\"date\":\"%s\"}",
                first ? "" : ",", name, (long)st.st_size, ver, date);
            first = 0;
        }
        closedir(d);
    }
    off += (size_t)snprintf(body + off, sizeof(body) - off,
                            "],\"staged\":{");
    const struct { const char *key; const char *path; } staged[] = {
        { "download", DL_FW }, { "upload", UP_FW },
    };
    for (size_t s = 0; s < 2; s++) {
        struct stat st;
        int have = stat(staged[s].path, &st) == 0 && st.st_size > 0;
        char ver[48] = "";
        if (have)
            fw_meta_version(staged[s].path, ver, sizeof(ver));
        off += (size_t)snprintf(body + off, sizeof(body) - off,
            "%s\"%s\":{\"present\":%s,\"bytes\":%ld,\"version\":\"%s\"}",
            s ? "," : "", staged[s].key, have ? "true" : "false",
            have ? (long)st.st_size : 0, ver);
    }
    snprintf(body + off, sizeof(body) - off, "}}");
    return reply_json(res, 200, body);
}

/* ------------------------------------------------------- boot select */

int cb_boot_select(const struct _u_request *req, struct _u_response *res,
                   void *user_data)
{
    (void)user_data;
    if (diag_running())
        return reply_err(res, 409, "a diagnostic is running");
    if (!machine_is_idle())
        return reply_err(res, 409, "machine is not idle");
    if (update_job_running())
        return reply_err(res, 409, "an update job is running");

    const struct slot_target *t = find_target(param(req, "target"));
    if (!t)
        return reply_err(res, 400,
                         "target must be sd, a, b, or legacy");
    const char *force = param(req, "force");

    char cmd[160], out[512];
    snprintf(cmd, sizeof(cmd), FFBOOT " %s -n%s 2>&1", t->ffboot_arg,
             (force && !strcmp(force, "1")) ? " -f" : "");
    int rc = run_cmd(out, sizeof(out), cmd);
    if (rc != 0) {
        char body[640];
        snprintf(body, sizeof(body),
                 "{\"error\":\"boot selection failed\",\"detail\":\"%s\"}",
                 out);
        return reply_json(res, 409, body);
    }
    char body[640];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"target\":\"%s\",\"detail\":\"%s\"}",
             t->name, out);
    return reply_json(res, 200, body);
}

int cb_system_reboot(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    if (diag_running())
        return reply_err(res, 409, "a diagnostic is running");
    if (!machine_is_idle())
        return reply_err(res, 409, "machine is not idle");
    if (update_job_running())
        return reply_err(res, 409, "an update job is running");
    const char *c = param(req, "confirm");
    if (!c || strcmp(c, "1"))
        return reply_err(res, 400, "confirm=1 required");
    sync();
    if (system("(sleep 1; reboot) >/dev/null 2>&1 &") != 0)
        return reply_err(res, 500, "cannot schedule reboot");
    return reply_json(res, 200, "{\"rebooting\":true}");
}

/* ------------------------------------------------------ release check */

int cb_update_check(const struct _u_request *req, struct _u_response *res,
                    void *user_data)
{
    (void)req;
    (void)user_data;

    /* HEAD through the redirect chain; the effective URL carries the
     * release tag. */
    char out[512];
    int rc = run_cmd(out, sizeof(out),
        "curl -sIL -o /dev/null -w '%{http_code} %{url_effective}' "
        "--max-time 20 " LATEST_URL " 2>/dev/null");
    if (rc != 0)
        return reply_err(res, 502, "release check failed (offline?)");

    char cur[48] = "";
    FILE *f = fopen("/etc/forgefirm-version", "r");
    if (f) {
        if (fgets(cur, sizeof(cur), f)) {
            char tmp[48];
            jsan(tmp, sizeof(tmp), cur);
            snprintf(cur, sizeof(cur), "%s", tmp);
        } else {
            cur[0] = '\0';
        }
        fclose(f);
    }

    int http = atoi(out);
    char ver[48] = "";
    char *m = strstr(out, "/download/");
    if (m) {
        m += 10;
        size_t o = 0;
        while (*m && *m != '/' && o + 1 < sizeof(ver))
            ver[o++] = *m++;
        ver[o] = '\0';
    }
    char body[256];
    if (http != 200 || !ver[0]) {
        /* 404 = no published release with a forgefirm.fw asset (the
         * expected state before the first release), distinct from a
         * transport/proxy error. */
        const char *detail = (http == 404 || http == 0)
            ? "no published release found"
            : "release server error";
        snprintf(body, sizeof(body),
                 "{\"available\":false,\"current\":\"%s\","
                 "\"detail\":\"%s (HTTP %d)\"}", cur, detail, http);
    } else
        snprintf(body, sizeof(body),
                 "{\"available\":true,\"version\":\"%s\","
                 "\"current\":\"%s\",\"new\":%s}",
                 ver, cur, strcmp(ver, cur) ? "true" : "false");
    return reply_json(res, 200, body);
}

/* --------------------------------------------------- download worker */

static void *dl_worker(void *arg)
{
    (void)arg;
    job_set_phase("downloading");
    pthread_mutex_lock(&mu);
    snprintf(progress_file, sizeof(progress_file), "%s", DL_FW);
    pthread_mutex_unlock(&mu);

    mkdir(DATA_DIR, 0755);
    unlink(DL_FW);
    char out[512];
    int rc = run_cmd(out, sizeof(out),
                     "curl -fSL --max-time 600 -o " DL_FW " " LATEST_URL
                     " 2>&1");
    if (rc != 0) {
        unlink(DL_FW);
        job_finish("{\"ok\":false,\"error\":\"download failed\","
                   "\"detail\":\"%s\"}", out);
        return NULL;
    }

    job_set_phase("verifying signature");
    if (fw_classify(DL_FW) != 2) {
        unlink(DL_FW);
        job_finish("{\"ok\":false,\"error\":\"signature verification "
                   "failed - archive discarded\"}");
        return NULL;
    }
    char ver[48];
    fw_meta_version(DL_FW, ver, sizeof(ver));
    struct stat st;
    long sz = stat(DL_FW, &st) == 0 ? (long)st.st_size : 0;
    job_finish("{\"ok\":true,\"file\":\"download\",\"version\":\"%s\","
               "\"bytes\":%ld}", ver, sz);
    return NULL;
}

int cb_update_download(const struct _u_request *req,
                       struct _u_response *res, void *user_data)
{
    (void)req;
    (void)user_data;
    return job_start_reply(res, job_start("download", dl_worker, NULL));
}

/* ------------------------------------------------------ apply worker */

struct apply_args {
    const struct slot_target *slot;
    char file[128];
    int allow_unsigned;
};

static void *apply_worker(void *argp)
{
    struct apply_args *a = argp;
    char out[512], cmd[512];

    job_set_phase("taking update lock");
    if (take_lock() != 0) {
        job_finish("{\"ok\":false,\"error\":\"update lock is held "
                   "(another update in progress?)\"}");
        free(a);
        return NULL;
    }

    job_set_phase("verifying archive");
    int cls = fw_classify(a->file);
    int use_key = cls == 2;
    if (cls < 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"not a usable fwup archive\"}");
        free(a);
        return NULL;
    }
    if (cls != 2 && !a->allow_unsigned) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"archive is not signed with "
                   "the ForgeFIRM release key (confirm_unsigned=1 to "
                   "apply anyway)\"}");
        free(a);
        return NULL;
    }

    job_set_phase("unmounting target");
    snprintf(cmd, sizeof(cmd),
             "for m in $(sed -n 's|^%s \\([^ ]*\\).*|\\1|p' /proc/mounts); "
             "do umount \"$m\" 2>/dev/null; done", a->slot->dev);
    run_cmd(NULL, 0, cmd);

    job_set_phase("writing slot");
    snprintf(cmd, sizeof(cmd),
             "fwup -a -q -d %s -i %s -t %s%s%s 2>&1",
             a->slot->dev, a->file, a->slot->task,
             use_key ? " -p " : "", use_key ? KEY_RELEASE : "");
    int rc = run_cmd(out, sizeof(out), cmd);
    if (rc != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"fwup apply failed - slot %s "
                   "is undefined until rewritten\",\"detail\":\"%s\"}",
                   a->slot->name, out);
        free(a);
        return NULL;
    }

    job_set_phase("verifying written slot");
    snprintf(cmd, sizeof(cmd),
             "mkdir -p /run/ffverify && "
             "mount -o ro -t ext4 %s /run/ffverify 2>&1 && "
             "cat /run/ffverify/etc/forgefirm-version "
             "/run/ffverify/etc/version 2>/dev/null | head -n 1 && "
             "test -f /run/ffverify/boot/zImage; "
             "rc=$?; umount /run/ffverify 2>/dev/null; exit $rc",
             a->slot->dev);
    rc = run_cmd(out, sizeof(out), cmd);
    drop_lock();
    if (rc != 0) {
        job_finish("{\"ok\":false,\"error\":\"written slot failed "
                   "verification\",\"detail\":\"%s\"}", out);
        free(a);
        return NULL;
    }
    job_finish("{\"ok\":true,\"slot\":\"%s\",\"version\":\"%s\","
               "\"signed\":%s}",
               a->slot->name, out, cls == 2 ? "true" : "false");
    free(a);
    return NULL;
}

int cb_update_apply(const struct _u_request *req, struct _u_response *res,
                    void *user_data)
{
    (void)user_data;

    const struct slot_target *t = find_target(param(req, "slot"));
    if (!t || !t->task)
        return reply_err(res, 400, "slot must be a or b");
    if (!strcmp(t->dev, booted_root()))
        return reply_err(res, 409,
                         "refusing to write the booted root slot");
    if (!slot_geometry_ok(t))
        return reply_err(res, 409,
                         "target slot is not the 200 MiB factory geometry");

    const char *file = param(req, "file");
    const char *path = NULL;
    if (file && !strcmp(file, "download"))
        path = DL_FW;
    else if (file && !strcmp(file, "upload"))
        path = UP_FW;
    else
        return reply_err(res, 400, "file must be download or upload");
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0)
        return reply_err(res, 404, "staged archive not found");

    struct apply_args *a = calloc(1, sizeof(*a));
    if (!a)
        return reply_err(res, 500, "out of memory");
    a->slot = t;
    snprintf(a->file, sizeof(a->file), "%s", path);
    const char *cu = param(req, "confirm_unsigned");
    a->allow_unsigned = cu && !strcmp(cu, "1");

    int rc = job_start("apply", apply_worker, a);
    if (rc != 0)
        free(a);
    return job_start_reply(res, rc);
}

/* ----------------------------------------------------------- upload */

int update_upload_sink(const struct _u_request *req, const char *key,
                       const char *filename, const char *content_type,
                       const char *transfer_encoding, const char *data,
                       uint64_t off, size_t size, void *user_data)
{
    (void)key;
    (void)filename;
    (void)content_type;
    (void)transfer_encoding;
    (void)user_data;
    if (!req->http_url || strncmp(req->http_url, "/update/upload", 14))
        return U_OK;                    /* not ours: ignore */

    pthread_mutex_lock(&up_mu);
    if (off == 0) {
        if (up_fp)
            fclose(up_fp);
        mkdir(DATA_DIR, 0755);
        up_fp = fopen(UP_FW, "wb");
        up_bytes = 0;
        up_error = up_fp ? 0 : 1;
    }
    if (up_fp && !up_error) {
        if (up_bytes + size > UPLOAD_MAX) {
            up_error = 1;
        } else if (size && fwrite(data, 1, size, up_fp) != size) {
            up_error = 1;
        } else {
            up_bytes += size;
        }
    }
    pthread_mutex_unlock(&up_mu);
    return U_OK;
}

int cb_update_upload(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)user_data;
    (void)req;

    pthread_mutex_lock(&up_mu);
    if (up_fp) {
        fclose(up_fp);
        up_fp = NULL;
    }
    int err = up_error;
    uint64_t bytes = up_bytes;
    pthread_mutex_unlock(&up_mu);

    if (err || bytes == 0) {
        unlink(UP_FW);
        return reply_err(res, 400,
                         err ? "upload failed or exceeded the size limit"
                             : "no file data received");
    }

    int cls = fw_classify(UP_FW);
    if (cls < 0) {
        unlink(UP_FW);
        return reply_err(res, 400, "not a usable fwup archive");
    }
    char ver[48];
    fw_meta_version(UP_FW, ver, sizeof(ver));
    char body[320];
    snprintf(body, sizeof(body),
             "{\"ok\":true,\"file\":\"upload\",\"bytes\":%llu,"
             "\"version\":\"%s\",\"signature\":\"%s\"}",
             (unsigned long long)bytes, ver,
             cls == 2 ? "forgefirm" : cls == 1 ? "glowforge" : "unsigned");
    return reply_json(res, 200, body);
}

/* --------------------------------------------------- factory restore */

struct restore_args {
    const struct slot_target *slot;
    char file[160];                  /* archive basename */
};

static void *restore_worker(void *argp)
{
    struct restore_args *a = argp;
    char cmd[640], out[512];

    job_set_phase("taking update lock");
    if (take_lock() != 0) {
        job_finish("{\"ok\":false,\"error\":\"update lock is held\"}");
        free(a);
        return NULL;
    }

    /* The manifest's md5 protects a years-old archive against bit rot
     * before it overwrites a slot. */
    job_set_phase("verifying archive checksum");
    snprintf(cmd, sizeof(cmd),
             "M=$(sed -n 's|.* %s md5=\\([0-9a-f]*\\)$|\\1|p' "
             ARCHIVE_DIR "/manifest | head -n 1); "
             "[ -n \"$M\" ] || exit 2; "
             "echo \"$M  " ARCHIVE_DIR "/%s\" | md5sum -c - >/dev/null 2>&1",
             a->file, a->file);
    int rc = run_cmd(out, sizeof(out), cmd);
    if (rc == 2) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"archive not in manifest\"}");
        free(a);
        return NULL;
    }
    if (rc != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"archive checksum MISMATCH - "
                   "not restoring from a corrupt archive\"}");
        free(a);
        return NULL;
    }

    job_set_phase("unmounting target");
    snprintf(cmd, sizeof(cmd),
             "for m in $(sed -n 's|^%s \\([^ ]*\\).*|\\1|p' /proc/mounts); "
             "do umount \"$m\" 2>/dev/null; done", a->slot->dev);
    run_cmd(NULL, 0, cmd);

    job_set_phase("writing factory image");
    snprintf(cmd, sizeof(cmd),
             "set -o pipefail; gzip -dc " ARCHIVE_DIR "/%s | "
             "dd of=%s bs=1M 2>&1 | tail -n 1", a->file, a->slot->dev);
    char shcmd[720];
    snprintf(shcmd, sizeof(shcmd), "sh -c '%s'", cmd);
    rc = run_cmd(out, sizeof(out), shcmd);
    if (rc != 0) {
        drop_lock();
        job_finish("{\"ok\":false,\"error\":\"restore write failed - "
                   "slot %s is undefined until rewritten\","
                   "\"detail\":\"%s\"}", a->slot->name, out);
        free(a);
        return NULL;
    }

    job_set_phase("verifying written slot");
    snprintf(cmd, sizeof(cmd),
             "mkdir -p /run/ffverify && "
             "mount -o ro -t ext4 %s /run/ffverify 2>&1 && "
             "cat /run/ffverify/etc/version 2>/dev/null && "
             "test -f /run/ffverify/boot/zImage; "
             "rc=$?; umount /run/ffverify 2>/dev/null; exit $rc",
             a->slot->dev);
    rc = run_cmd(out, sizeof(out), cmd);
    drop_lock();
    if (rc != 0) {
        job_finish("{\"ok\":false,\"error\":\"restored slot failed "
                   "verification\",\"detail\":\"%s\"}", out);
        free(a);
        return NULL;
    }
    job_finish("{\"ok\":true,\"slot\":\"%s\",\"factory_version\":\"%s\"}",
               a->slot->name, out);
    free(a);
    return NULL;
}

int cb_restore_factory(const struct _u_request *req,
                       struct _u_response *res, void *user_data)
{
    (void)user_data;

    const char *source = param(req, "source");
    if (source && !strcmp(source, "cloud"))
        return reply_err(res, 501,
            "cloud restore is not implemented yet - use an archive");

    const struct slot_target *t = find_target(param(req, "slot"));
    if (!t || !t->task)
        return reply_err(res, 400, "slot must be a or b");
    if (!strcmp(t->dev, booted_root()))
        return reply_err(res, 409,
                         "refusing to write the booted root slot");
    if (!slot_geometry_ok(t))
        return reply_err(res, 409,
                         "target slot is not the 200 MiB factory geometry");

    const char *file = param(req, "file");
    if (!file || strchr(file, '/') || strstr(file, "..") ||
        strncmp(file, "factory-rootfs-", 15))
        return reply_err(res, 400,
                         "file must be a factory-rootfs archive name");
    char path[256];
    snprintf(path, sizeof(path), ARCHIVE_DIR "/%s", file);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size == 0)
        return reply_err(res, 404, "archive not found");

    struct restore_args *a = calloc(1, sizeof(*a));
    if (!a)
        return reply_err(res, 500, "out of memory");
    a->slot = t;
    snprintf(a->file, sizeof(a->file), "%s", file);

    int rc = job_start("restore", restore_worker, a);
    if (rc != 0)
        free(a);
    return job_start_reply(res, rc);
}

/* ------------------------------------------------------------ status */

int cb_update_status(const struct _u_request *req, struct _u_response *res,
                     void *user_data)
{
    (void)req;
    (void)user_data;
    char body[1280];
    pthread_mutex_lock(&mu);
    long prog = -1;
    if (job_running && progress_file[0]) {
        struct stat st;
        if (stat(progress_file, &st) == 0)
            prog = (long)st.st_size;
    }
    snprintf(body, sizeof(body),
             "{\"running\":%s,\"kind\":\"%s\",\"phase\":\"%s\","
             "\"elapsed\":%ld,\"progress_bytes\":%ld,\"result\":%s}",
             job_running ? "true" : "false", job_kind, job_phase,
             job_running ? (long)(time(NULL) - job_started) : 0, prog,
             job_result[0] ? job_result : "null");
    pthread_mutex_unlock(&mu);
    return reply_json(res, 200, body);
}

/* -------------------------------------------------------------- init */

void update_init(void)
{
    mkdir(DATA_DIR, 0755);
    /* A stale lock file from a crash is harmless: flock state dies with
     * the holder. Partial staged files are re-verified before use. */
}

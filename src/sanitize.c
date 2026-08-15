/*
 * sanitize.c - forgectrl: log-export redaction
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Two layers, applied in order to every line:
 *
 *  1. Known values (exact, boundary-delimited): the machine's serial and
 *     hostname, the cloud credentials, the panel token, the WiFi SSID
 *     and passphrase - whatever the caller registers. Exact matching is
 *     far more reliable than guessing shapes, so this layer carries the
 *     identifying data that matters most.
 *  2. Pattern classes: bearer/basic credentials, JWTs, key=value
 *     secrets, e-mail addresses, MAC addresses, IPv4/IPv6 addresses
 *     (loopback and unspecified kept), long hex blobs (>= 32) and long
 *     base64-like blobs (>= 40). Each distinct value in a class gets a
 *     stable number for the life of the sanitizer, so "<IP-2>" means
 *     the same host everywhere in one export.
 *
 * Over-redaction is accepted where the alternative is a leak: a version
 * string that happens to look like an address is a small price.
 */
#define _GNU_SOURCE
#include "sanitize.h"

#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---------------------------------------------------- string builder */

struct sb {
    char *p;
    size_t len, cap;
    int oom;
};

static void sb_init(struct sb *b)
{
    b->p = NULL;
    b->len = b->cap = 0;
    b->oom = 0;
}

static void sb_add(struct sb *b, const char *s, size_t n)
{
    if (b->oom)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1)
            nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void sb_adds(struct sb *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static char *sb_take(struct sb *b)
{
    if (b->oom) {
        free(b->p);
        return NULL;
    }
    if (!b->p)
        return strdup("");
    return b->p;
}

/* ------------------------------------------------------------ object */

struct known {
    char *tag;
    char *val;
    size_t len;
};

struct class {
    const char *tag;
    char **vals;
    size_t n, cap;
};

struct stat {
    char tag[32];
    unsigned long n;
};

enum { CL_IP, CL_IP6, CL_MAC, CL_EMAIL, CL_HEX, CL_B64, CL_COUNT };

struct sanitizer {
    struct known *known;
    size_t nknown, capknown;
    struct class cls[CL_COUNT];
    struct stat *stats;
    size_t nstats, capstats;
    regex_t re_bearer, re_basic, re_jwt, re_kv, re_email, re_mac, re_ip4;
    int compiled;
};

static void count(sanitizer_t *s, const char *tag)
{
    for (size_t i = 0; i < s->nstats; i++)
        if (!strcmp(s->stats[i].tag, tag)) {
            s->stats[i].n++;
            return;
        }
    if (s->nstats == s->capstats) {
        size_t nc = s->capstats ? s->capstats * 2 : 16;
        struct stat *ns = realloc(s->stats, nc * sizeof(*ns));
        if (!ns)
            return;
        s->stats = ns;
        s->capstats = nc;
    }
    snprintf(s->stats[s->nstats].tag, sizeof(s->stats[0].tag), "%s", tag);
    s->stats[s->nstats].n = 1;
    s->nstats++;
}

/* Stable per-class numbering: the same value always maps to the same
 * placeholder within one sanitizer. */
static size_t class_index(struct class *c, const char *val, size_t len)
{
    for (size_t i = 0; i < c->n; i++)
        if (strlen(c->vals[i]) == len && !memcmp(c->vals[i], val, len))
            return i + 1;
    if (c->n == c->cap) {
        size_t nc = c->cap ? c->cap * 2 : 32;
        char **nv = realloc(c->vals, nc * sizeof(*nv));
        if (!nv)
            return c->n + 1;
        c->vals = nv;
        c->cap = nc;
    }
    c->vals[c->n] = strndup(val, len);
    if (!c->vals[c->n])
        return c->n + 1;
    c->n++;
    return c->n;
}

static void placeholder(sanitizer_t *s, int cl, const char *val, size_t len,
                        struct sb *out)
{
    char ph[48];
    size_t idx = class_index(&s->cls[cl], val, len);
    snprintf(ph, sizeof(ph), "<%s-%zu>", s->cls[cl].tag, idx);
    sb_adds(out, ph);
    count(s, s->cls[cl].tag);
}

sanitizer_t *san_new(void)
{
    sanitizer_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    static const char *tags[CL_COUNT] = {
        [CL_IP] = "IP", [CL_IP6] = "IP6", [CL_MAC] = "MAC",
        [CL_EMAIL] = "EMAIL", [CL_HEX] = "HEX", [CL_B64] = "B64",
    };
    for (int i = 0; i < CL_COUNT; i++)
        s->cls[i].tag = tags[i];

    int rc = 0;
    rc |= regcomp(&s->re_bearer,
                  "[Bb][Ee][Aa][Rr][Ee][Rr][ ]+[A-Za-z0-9._~+/=-]{8,}",
                  REG_EXTENDED);
    rc |= regcomp(&s->re_basic,
                  "[Bb][Aa][Ss][Ii][Cc][ ]+[A-Za-z0-9+/=]{8,}",
                  REG_EXTENDED);
    rc |= regcomp(&s->re_jwt,
                  "eyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}",
                  REG_EXTENDED);
    /* key = value / key: value / key=\"value\" - the key kept, the
     * value replaced. Group 1 = the key with its leading boundary and
     * separator, group 3 = an optional opening quote. */
    rc |= regcomp(&s->re_kv,
                  "((^|[^A-Za-z0-9_])"
                  "(token|access_token|ws_token|auth_token|refresh_token|"
                  "id_token|password|passwd|pwd|passphrase|secret|"
                  "client_secret|api_key|apikey|secret_key|private_key|psk|"
                  "authorization|session_id|sessionid|cookie|set-cookie)"
                  "[ ]*[=:][ ]*)(\"?)([^&[:space:]\"',;]+)",
                  REG_EXTENDED | REG_ICASE);
    rc |= regcomp(&s->re_email,
                  "[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}",
                  REG_EXTENDED);
    rc |= regcomp(&s->re_mac,
                  "(^|[^0-9A-Fa-f:])([0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5})",
                  REG_EXTENDED);
    rc |= regcomp(&s->re_ip4,
                  "(^|[^0-9.])(([0-9]{1,3}\\.){3}[0-9]{1,3})",
                  REG_EXTENDED);
    if (rc) {
        san_free(s);
        return NULL;
    }
    s->compiled = 1;
    return s;
}

void san_free(sanitizer_t *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->nknown; i++) {
        free(s->known[i].tag);
        free(s->known[i].val);
    }
    free(s->known);
    for (int c = 0; c < CL_COUNT; c++) {
        for (size_t i = 0; i < s->cls[c].n; i++)
            free(s->cls[c].vals[i]);
        free(s->cls[c].vals);
    }
    free(s->stats);
    if (s->compiled) {
        regfree(&s->re_bearer);
        regfree(&s->re_basic);
        regfree(&s->re_jwt);
        regfree(&s->re_kv);
        regfree(&s->re_email);
        regfree(&s->re_mac);
        regfree(&s->re_ip4);
    }
    free(s);
}

void san_add_known(sanitizer_t *s, const char *tag, const char *value)
{
    if (!s || !tag || !value)
        return;
    size_t len = strlen(value);
    int all_digits = 1;
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char)value[i]))
            all_digits = 0;
    if (len < 3 || (all_digits && len < 5))
        return;
    for (size_t i = 0; i < s->nknown; i++)
        if (!strcmp(s->known[i].val, value))
            return;
    if (s->nknown == s->capknown) {
        size_t nc = s->capknown ? s->capknown * 2 : 8;
        struct known *nk = realloc(s->known, nc * sizeof(*nk));
        if (!nk)
            return;
        s->known = nk;
        s->capknown = nc;
    }
    struct known k = { strdup(tag), strdup(value), len };
    if (!k.tag || !k.val) {
        free(k.tag);
        free(k.val);
        return;
    }
    /* Longest first, so a value that contains another is replaced
     * whole rather than in pieces. */
    size_t pos = s->nknown;
    while (pos > 0 && s->known[pos - 1].len < len) {
        s->known[pos] = s->known[pos - 1];
        pos--;
    }
    s->known[pos] = k;
    s->nknown++;
}

/* --------------------------------------------------------- passes */

static int is_word(int c)
{
    return isalnum(c) || c == '_';
}

/* Pass 1: known values, exact and boundary-delimited. */
static char *pass_known(sanitizer_t *s, const char *in)
{
    if (!s->nknown)
        return strdup(in);
    char *cur = strdup(in);
    if (!cur)
        return NULL;
    for (size_t k = 0; k < s->nknown; k++) {
        const struct known *kn = &s->known[k];
        if (!strstr(cur, kn->val))
            continue;
        struct sb out;
        sb_init(&out);
        const char *p = cur;
        for (;;) {
            const char *m = strstr(p, kn->val);
            if (!m) {
                sb_adds(&out, p);
                break;
            }
            int lb = (m == cur) || !is_word((unsigned char)m[-1]);
            int rb = !is_word((unsigned char)m[kn->len]);
            sb_add(&out, p, (size_t)(m - p));
            if (lb && rb) {
                sb_adds(&out, "<");
                sb_adds(&out, kn->tag);
                sb_adds(&out, ">");
                count(s, kn->tag);
                p = m + kn->len;
            } else {
                sb_add(&out, m, 1);
                p = m + 1;
            }
        }
        free(cur);
        cur = sb_take(&out);
        if (!cur)
            return NULL;
    }
    return cur;
}

/* Generic regex-driven pass. cb() writes the replacement for a match
 * into out and returns 1, or returns 0 to leave it (scanning resumes
 * one character on). */
typedef int (*match_cb)(sanitizer_t *s, const char *line,
                        const regmatch_t *m, struct sb *out);

static char *pass_regex(sanitizer_t *s, const char *in, regex_t *re,
                        size_t nmatch, match_cb cb)
{
    struct sb out;
    sb_init(&out);
    const char *p = in;
    regmatch_t m[8];
    if (nmatch > 8)
        nmatch = 8;
    while (*p) {
        int flags = (p != in) ? REG_NOTBOL : 0;
        if (regexec(re, p, nmatch, m, flags) != 0) {
            sb_adds(&out, p);
            break;
        }
        sb_add(&out, p, (size_t)m[0].rm_so);
        if (cb(s, p, m, &out)) {
            p += m[0].rm_eo;
        } else {
            sb_add(&out, p + m[0].rm_so, 1);
            p += m[0].rm_so + 1;
        }
        if (m[0].rm_eo == 0 && m[0].rm_so == 0) {
            /* empty match cannot happen with these patterns; guard */
            sb_add(&out, p, 1);
            p++;
        }
    }
    return sb_take(&out);
}

static int cb_bearer(sanitizer_t *s, const char *l, const regmatch_t *m,
                     struct sb *out)
{
    (void)l;
    (void)m;
    sb_adds(out, "Bearer <TOKEN>");
    count(s, "TOKEN");
    return 1;
}

static int cb_basic(sanitizer_t *s, const char *l, const regmatch_t *m,
                    struct sb *out)
{
    (void)l;
    (void)m;
    sb_adds(out, "Basic <TOKEN>");
    count(s, "TOKEN");
    return 1;
}

static int cb_jwt(sanitizer_t *s, const char *l, const regmatch_t *m,
                  struct sb *out)
{
    (void)l;
    (void)m;
    sb_adds(out, "<JWT>");
    count(s, "JWT");
    return 1;
}

static int cb_kv(sanitizer_t *s, const char *l, const regmatch_t *m,
                 struct sb *out)
{
    /* keep key + separator (group 1) and the opening quote (group 4),
     * replace the value (group 5) */
    sb_add(out, l + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
    sb_add(out, l + m[4].rm_so, (size_t)(m[4].rm_eo - m[4].rm_so));
    /* An already-redacted value (an earlier pass) needs no second
     * placeholder. */
    const char *v = l + m[5].rm_so;
    size_t vlen = (size_t)(m[5].rm_eo - m[5].rm_so);
    if (vlen >= 2 && v[0] == '<' && v[vlen - 1] == '>') {
        sb_add(out, v, vlen);
        return 1;
    }
    /* "Authorization: Bearer <TOKEN>" - the scheme word is not the
     * secret; the earlier pass already replaced the credential. */
    if ((vlen == 6 && !strncasecmp(v, "bearer", 6)) ||
        (vlen == 5 && !strncasecmp(v, "basic", 5))) {
        sb_add(out, v, vlen);
        return 1;
    }
    sb_adds(out, "<REDACTED>");
    count(s, "REDACTED");
    return 1;
}

static int cb_email(sanitizer_t *s, const char *l, const regmatch_t *m,
                    struct sb *out)
{
    placeholder(s, CL_EMAIL, l + m[0].rm_so,
                (size_t)(m[0].rm_eo - m[0].rm_so), out);
    return 1;
}

static int cb_mac(sanitizer_t *s, const char *l, const regmatch_t *m,
                  struct sb *out)
{
    /* trailing boundary: not another hex pair / colon (longer runs are
     * not MACs - leave them for the IPv6 scanner) */
    char after = l[m[2].rm_eo];
    if (isxdigit((unsigned char)after) || after == ':')
        return 0;
    sb_add(out, l + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
    placeholder(s, CL_MAC, l + m[2].rm_so,
                (size_t)(m[2].rm_eo - m[2].rm_so), out);
    return 1;
}

static int ip4_keep(const char *ip, size_t len)
{
    char buf[20];
    if (len >= sizeof(buf))
        return 1;
    memcpy(buf, ip, len);
    buf[len] = '\0';
    unsigned a, b, c, d;
    if (sscanf(buf, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 1;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return 1;                       /* not an address */
    if (a == 127)
        return 1;                       /* loopback */
    if (a == 0 && b == 0 && c == 0 && d == 0)
        return 1;                       /* unspecified */
    if (a == 255 && b == 255 && c == 255 && d == 255)
        return 1;                       /* broadcast */
    return 0;
}

static int cb_ip4(sanitizer_t *s, const char *l, const regmatch_t *m,
                  struct sb *out)
{
    const char *ip = l + m[2].rm_so;
    size_t len = (size_t)(m[2].rm_eo - m[2].rm_so);
    char after = l[m[2].rm_eo];
    if (isdigit((unsigned char)after))
        return 0;
    if (after == '.' && isdigit((unsigned char)l[m[2].rm_eo + 1]))
        return 0;                       /* 5+ dotted groups: not IPv4 */
    if (ip4_keep(ip, len))
        return 0;
    sb_add(out, l + m[1].rm_so, (size_t)(m[1].rm_eo - m[1].rm_so));
    placeholder(s, CL_IP, ip, len, out);
    return 1;
}

/* IPv6, hand-scanned: a run of hex digits and colons that either
 * contains "::" or has exactly eight groups, every group 1-4 hex
 * digits. Timestamps (two colons, no "::") and MACs (six groups) do
 * not qualify. "::" and "::1" are kept. */
static int ip6_valid(const char *p, size_t len)
{
    size_t colons = 0, groups = 0, glen = 0;
    int dbl = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == ':') {
            colons++;
            if (i + 1 < len && p[i + 1] == ':')
                dbl = 1;
            if (glen > 4)
                return 0;
            if (glen)
                groups++;
            glen = 0;
        } else {
            glen++;
        }
    }
    if (glen > 4)
        return 0;
    if (glen)
        groups++;
    if (colons < 2 || colons > 7)
        return 0;
    if (dbl)
        return 1;
    return groups == 8;
}

static char *pass_ip6(sanitizer_t *s, const char *in)
{
    struct sb out;
    sb_init(&out);
    const char *p = in;
    while (*p) {
        if (!(isxdigit((unsigned char)*p) || *p == ':')) {
            const char *r = p + 1;
            while (*r && !(isxdigit((unsigned char)*r) || *r == ':'))
                r++;
            sb_add(&out, p, (size_t)(r - p));
            p = r;
            continue;
        }
        const char *q = p;
        while (isxdigit((unsigned char)*q) || *q == ':')
            q++;
        size_t len = (size_t)(q - p);
        int lb = (p == in) || !(isalnum((unsigned char)p[-1]) ||
                                p[-1] == '.' || p[-1] == ':');
        int rb = !(isalnum((unsigned char)*q) || *q == '.');
        int keep = (len == 2 && !memcmp(p, "::", 2)) ||
                   (len == 3 && !memcmp(p, "::1", 3));
        if (lb && rb && !keep && memchr(p, ':', len) && ip6_valid(p, len)) {
            placeholder(s, CL_IP6, p, len, &out);
        } else {
            sb_add(&out, p, len);
        }
        p = q;
    }
    return sb_take(&out);
}

/* Long hex blobs (>= 32): tokens, hashes, keys. */
static char *pass_hex(sanitizer_t *s, const char *in)
{
    struct sb out;
    sb_init(&out);
    const char *p = in;
    while (*p) {
        if (!isxdigit((unsigned char)*p)) {
            const char *r = p + 1;
            while (*r && !isxdigit((unsigned char)*r))
                r++;
            sb_add(&out, p, (size_t)(r - p));
            p = r;
            continue;
        }
        const char *q = p;
        while (isxdigit((unsigned char)*q))
            q++;
        size_t len = (size_t)(q - p);
        int lb = (p == in) || !is_word((unsigned char)p[-1]);
        int rb = !is_word((unsigned char)*q);
        if (len >= 32 && lb && rb)
            placeholder(s, CL_HEX, p, len, &out);
        else
            sb_add(&out, p, len);
        p = q;
    }
    return sb_take(&out);
}

/* Long base64-like blobs (>= 40, letters and digits both present, no
 * '/' so paths are left alone). */
static int is_b64(int c)
{
    return isalnum(c) || c == '+' || c == '=' || c == '_' || c == '-';
}

static char *pass_b64(sanitizer_t *s, const char *in)
{
    struct sb out;
    sb_init(&out);
    const char *p = in;
    while (*p) {
        if (!is_b64((unsigned char)*p)) {
            const char *r = p + 1;
            while (*r && !is_b64((unsigned char)*r))
                r++;
            sb_add(&out, p, (size_t)(r - p));
            p = r;
            continue;
        }
        const char *q = p;
        int letters = 0, digits = 0;
        while (is_b64((unsigned char)*q)) {
            if (isalpha((unsigned char)*q))
                letters = 1;
            else if (isdigit((unsigned char)*q))
                digits = 1;
            q++;
        }
        size_t len = (size_t)(q - p);
        int lb = (p == in) || !(is_word((unsigned char)p[-1]) ||
                                p[-1] == '<');
        int rb = !(is_word((unsigned char)*q) || *q == '>');
        if (len >= 40 && letters && digits && lb && rb)
            placeholder(s, CL_B64, p, len, &out);
        else
            sb_add(&out, p, len);
        p = q;
    }
    return sb_take(&out);
}

/* Cheap pre-checks: a regex pass runs only when the line can contain a
 * match at all (a substring the pattern requires). POSIX regexec is the
 * expensive part on the target; most log lines have no '@', no "eyJ",
 * no credential keyword, so most passes are skipped outright. */
static int has_ci(const char *s, const char *word)
{
    return strcasestr(s, word) != NULL;
}

static int may_have_kv(const char *s)
{
    static const char *const words[] = {
        "token", "password", "passwd", "pwd", "passphrase", "secret",
        "api_key", "apikey", "secret_key", "private_key", "psk",
        "authorization", "session", "cookie",
    };
    if (!strchr(s, '=') && !strchr(s, ':'))
        return 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++)
        if (has_ci(s, words[i]))
            return 1;
    return 0;
}

static size_t count_char(const char *s, int c)
{
    size_t n = 0;
    for (; (s = strchr(s, c)) != NULL; s++)
        n++;
    return n;
}

static int may_have_ip4(const char *s)
{
    /* a digit, a dot, a digit somewhere */
    for (const char *p = s; (p = strchr(p, '.')) != NULL; p++)
        if (p > s && isdigit((unsigned char)p[-1]) && isdigit((unsigned char)p[1]))
            return 1;
    return 0;
}

char *san_line(sanitizer_t *s, const char *line)
{
    if (!s || !line)
        return NULL;
    char *cur = pass_known(s, line);
    struct step {
        regex_t *re;
        size_t nmatch;
        match_cb cb;
        int (*maybe)(const char *);
    } steps[] = {
        { &s->re_bearer, 1, cb_bearer, NULL },
        { &s->re_basic, 1, cb_basic, NULL },
        { &s->re_jwt, 1, cb_jwt, NULL },
        { &s->re_kv, 6, cb_kv, may_have_kv },
        { &s->re_email, 1, cb_email, NULL },
        { &s->re_mac, 4, cb_mac, NULL },
        { &s->re_ip4, 4, cb_ip4, may_have_ip4 },
    };
    for (size_t i = 0; cur && i < sizeof(steps) / sizeof(*steps); i++) {
        int run = 1;
        if (steps[i].re == &s->re_bearer)
            run = has_ci(cur, "bearer");
        else if (steps[i].re == &s->re_basic)
            run = has_ci(cur, "basic");
        else if (steps[i].re == &s->re_jwt)
            run = strstr(cur, "eyJ") != NULL;
        else if (steps[i].re == &s->re_email)
            run = strchr(cur, '@') != NULL;
        else if (steps[i].re == &s->re_mac)
            run = count_char(cur, ':') >= 5;
        else if (steps[i].maybe)
            run = steps[i].maybe(cur);
        if (!run)
            continue;
        char *next = pass_regex(s, cur, steps[i].re, steps[i].nmatch,
                                steps[i].cb);
        free(cur);
        cur = next;
    }
    if (cur && count_char(cur, ':') >= 2) {
        char *next = pass_ip6(s, cur);
        free(cur);
        cur = next;
    }
    if (cur) {
        char *next = pass_hex(s, cur);
        free(cur);
        cur = next;
    }
    if (cur) {
        char *next = pass_b64(s, cur);
        free(cur);
        cur = next;
    }
    return cur;
}

unsigned long san_total(const sanitizer_t *s)
{
    unsigned long t = 0;
    for (size_t i = 0; s && i < s->nstats; i++)
        t += s->stats[i].n;
    return t;
}

void san_report(const sanitizer_t *s, FILE *out)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->nstats; i++)
        fprintf(out, "<%s>: %lu\n", s->stats[i].tag, s->stats[i].n);
}

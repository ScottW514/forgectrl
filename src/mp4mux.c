/*
 * mp4mux.c - fragmented MP4 wrapping for the H.264 stream
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * ISO/IEC 14496-12 boxes, written big-endian into a growable buffer.
 * The layout is the minimal one MSE requires: ftyp + moov (with mvex,
 * announcing movie fragments) once, then mfhd/tfhd/tfdt/trun per frame
 * with default-base-is-moof addressing, one sample per fragment.
 */
#include "mp4mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPS_MAX 256
#define PPS_MAX 64

struct mp4mux {
    int      w, h;
    uint8_t  sps[SPS_MAX];
    size_t   sps_len;
    uint8_t  pps[PPS_MAX];
    size_t   pps_len;
    char     codec[24];
};

/* ------------------------------------------------- growable byte buffer */

struct bb {
    uint8_t *p;
    size_t   len, cap;
    int      oom;
};

static void bb_put(struct bb *b, const void *data, size_t n)
{
    if (b->oom)
        return;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1024;
        while (nc < b->len + n)
            nc *= 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
}

static void u8(struct bb *b, uint8_t v)   { bb_put(b, &v, 1); }
static void u16(struct bb *b, uint16_t v)
{
    uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    bb_put(b, x, 2);
}
static void u32(struct bb *b, uint32_t v)
{
    uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8), (uint8_t)v };
    bb_put(b, x, 4);
}
static void u64f(struct bb *b, uint64_t v)
{
    u32(b, (uint32_t)(v >> 32));
    u32(b, (uint32_t)v);
}
static void tag(struct bb *b, const char *t) { bb_put(b, t, 4); }

/* Reserve a box header, returning the offset to patch its size into. */
static size_t box_open(struct bb *b, const char *t)
{
    size_t at = b->len;
    u32(b, 0);
    tag(b, t);
    return at;
}
static void box_close(struct bb *b, size_t at)
{
    if (b->oom)
        return;
    uint32_t sz = (uint32_t)(b->len - at);
    b->p[at] = (uint8_t)(sz >> 24);
    b->p[at + 1] = (uint8_t)(sz >> 16);
    b->p[at + 2] = (uint8_t)(sz >> 8);
    b->p[at + 3] = (uint8_t)sz;
}

/* ------------------------------------------------------ Annex B parsing */

/* Call cb(nal, len) for each NAL in an Annex B unit. */
static void each_nal(const uint8_t *au, size_t len,
                     void (*cb)(void *, const uint8_t *, size_t),
                     void *ctx)
{
    size_t i = 0;
    while (i + 3 < len) {
        if (au[i] == 0 && au[i + 1] == 0 &&
            (au[i + 2] == 1 ||
             (au[i + 2] == 0 && i + 4 < len && au[i + 3] == 1))) {
            size_t start = i + (au[i + 2] == 1 ? 3 : 4);
            size_t j = start;
            while (j + 3 < len &&
                   !(au[j] == 0 && au[j + 1] == 0 &&
                     (au[j + 2] == 1 || (au[j + 2] == 0 && j + 4 <= len &&
                                         au[j + 3] == 1))))
                j++;
            size_t end = (j + 3 < len) ? j : len;
            if (end > start)
                cb(ctx, au + start, end - start);
            i = end;
        } else {
            i++;
        }
    }
}

struct param_scan {
    mp4mux_t *m;
};

static void scan_cb(void *ctx, const uint8_t *nal, size_t n)
{
    mp4mux_t *m = ((struct param_scan *)ctx)->m;
    switch (nal[0] & 0x1F) {
    case 7:
        if (n <= SPS_MAX) {
            memcpy(m->sps, nal, n);
            m->sps_len = n;
            snprintf(m->codec, sizeof(m->codec), "avc1.%02x%02x%02x",
                     nal[1], nal[2], nal[3]);
        }
        break;
    case 8:
        if (n <= PPS_MAX) {
            memcpy(m->pps, nal, n);
            m->pps_len = n;
        }
        break;
    }
}

/* -------------------------------------------------------------- public */

mp4mux_t *mp4mux_new(int w, int h)
{
    mp4mux_t *m = calloc(1, sizeof(*m));
    if (m) {
        m->w = w;
        m->h = h;
    }
    return m;
}

void mp4mux_free(mp4mux_t *m)
{
    free(m);
}

int mp4mux_feed_params(mp4mux_t *m, const uint8_t *au, size_t len)
{
    struct param_scan ps = { m };
    each_nal(au, len, scan_cb, &ps);
    return m->sps_len > 0 && m->pps_len > 0;
}

const char *mp4mux_codec(mp4mux_t *m)
{
    return m->codec;
}

size_t mp4mux_params_annexb(mp4mux_t *m, uint8_t *dst, size_t cap)
{
    static const uint8_t sc[4] = { 0, 0, 0, 1 };
    size_t need = 8 + m->sps_len + m->pps_len;
    if (!m->sps_len || !m->pps_len || cap < need)
        return 0;
    memcpy(dst, sc, 4);
    memcpy(dst + 4, m->sps, m->sps_len);
    memcpy(dst + 4 + m->sps_len, sc, 4);
    memcpy(dst + 8 + m->sps_len, m->pps, m->pps_len);
    return need;
}

uint8_t *mp4mux_init_segment(mp4mux_t *m, size_t *len)
{
    if (!m->sps_len || !m->pps_len)
        return NULL;
    struct bb b = {0};

    size_t ftyp = box_open(&b, "ftyp");
    tag(&b, "isom");
    u32(&b, 0x200);
    tag(&b, "isom"); tag(&b, "iso5"); tag(&b, "iso2");
    tag(&b, "avc1"); tag(&b, "mp41");
    box_close(&b, ftyp);

    size_t moov = box_open(&b, "moov");

    size_t mvhd = box_open(&b, "mvhd");
    u32(&b, 0);                         /* version 0, flags */
    u32(&b, 0); u32(&b, 0);             /* creation, modification */
    u32(&b, MP4MUX_TIMESCALE);
    u32(&b, 0);                         /* duration: live, unknown */
    u32(&b, 0x00010000);                /* rate 1.0 */
    u16(&b, 0x0100);                    /* volume 1.0 */
    u16(&b, 0);
    u32(&b, 0); u32(&b, 0);             /* reserved */
    u32(&b, 0x00010000); u32(&b, 0); u32(&b, 0);        /* unity matrix */
    u32(&b, 0); u32(&b, 0x00010000); u32(&b, 0);
    u32(&b, 0); u32(&b, 0); u32(&b, 0x40000000);
    for (int i = 0; i < 6; i++)
        u32(&b, 0);                     /* pre_defined */
    u32(&b, 2);                         /* next track id */
    box_close(&b, mvhd);

    size_t trak = box_open(&b, "trak");
    size_t tkhd = box_open(&b, "tkhd");
    u32(&b, 0x00000003);                /* v0, enabled | in movie */
    u32(&b, 0); u32(&b, 0);
    u32(&b, 1);                         /* track id */
    u32(&b, 0);
    u32(&b, 0);                         /* duration */
    u32(&b, 0); u32(&b, 0);
    u16(&b, 0); u16(&b, 0); u16(&b, 0); u16(&b, 0);
    u32(&b, 0x00010000); u32(&b, 0); u32(&b, 0);
    u32(&b, 0); u32(&b, 0x00010000); u32(&b, 0);
    u32(&b, 0); u32(&b, 0); u32(&b, 0x40000000);
    u32(&b, (uint32_t)m->w << 16);      /* 16.16 */
    u32(&b, (uint32_t)m->h << 16);
    box_close(&b, tkhd);

    size_t mdia = box_open(&b, "mdia");
    size_t mdhd = box_open(&b, "mdhd");
    u32(&b, 0);
    u32(&b, 0); u32(&b, 0);
    u32(&b, MP4MUX_TIMESCALE);
    u32(&b, 0);
    u16(&b, 0x55C4);                    /* language: und */
    u16(&b, 0);
    box_close(&b, mdhd);

    size_t hdlr = box_open(&b, "hdlr");
    u32(&b, 0);
    u32(&b, 0);
    tag(&b, "vide");
    u32(&b, 0); u32(&b, 0); u32(&b, 0);
    bb_put(&b, "forgectrl", 10);        /* includes NUL */
    box_close(&b, hdlr);

    size_t minf = box_open(&b, "minf");
    size_t vmhd = box_open(&b, "vmhd");
    u32(&b, 1);                         /* v0, flags 1 */
    u16(&b, 0); u16(&b, 0); u16(&b, 0); u16(&b, 0);
    box_close(&b, vmhd);

    size_t dinf = box_open(&b, "dinf");
    size_t dref = box_open(&b, "dref");
    u32(&b, 0);
    u32(&b, 1);
    size_t url = box_open(&b, "url ");
    u32(&b, 1);                         /* self-contained */
    box_close(&b, url);
    box_close(&b, dref);
    box_close(&b, dinf);

    size_t stbl = box_open(&b, "stbl");
    size_t stsd = box_open(&b, "stsd");
    u32(&b, 0);
    u32(&b, 1);
    size_t avc1 = box_open(&b, "avc1");
    for (int i = 0; i < 6; i++)
        u8(&b, 0);                      /* reserved */
    u16(&b, 1);                         /* data reference index */
    u16(&b, 0); u16(&b, 0);
    u32(&b, 0); u32(&b, 0); u32(&b, 0);
    u16(&b, (uint16_t)m->w);
    u16(&b, (uint16_t)m->h);
    u32(&b, 0x00480000);                /* 72 dpi */
    u32(&b, 0x00480000);
    u32(&b, 0);
    u16(&b, 1);                         /* frame count */
    for (int i = 0; i < 32; i++)
        u8(&b, 0);                      /* compressor name */
    u16(&b, 0x0018);                    /* depth 24 */
    u16(&b, 0xFFFF);                    /* pre_defined */
    size_t avcC = box_open(&b, "avcC");
    u8(&b, 1);                          /* configuration version */
    u8(&b, m->sps[1]);                  /* profile */
    u8(&b, m->sps[2]);                  /* profile compat */
    u8(&b, m->sps[3]);                  /* level */
    u8(&b, 0xFF);                       /* 4-byte NAL lengths */
    u8(&b, 0xE1);                       /* 1 SPS */
    u16(&b, (uint16_t)m->sps_len);
    bb_put(&b, m->sps, m->sps_len);
    u8(&b, 1);                          /* 1 PPS */
    u16(&b, (uint16_t)m->pps_len);
    bb_put(&b, m->pps, m->pps_len);
    box_close(&b, avcC);
    box_close(&b, avc1);
    box_close(&b, stsd);
    /* Empty sample tables: every sample arrives in fragments. */
    size_t x;
    x = box_open(&b, "stts"); u32(&b, 0); u32(&b, 0); box_close(&b, x);
    x = box_open(&b, "stsc"); u32(&b, 0); u32(&b, 0); box_close(&b, x);
    x = box_open(&b, "stsz"); u32(&b, 0); u32(&b, 0); u32(&b, 0);
    box_close(&b, x);
    x = box_open(&b, "stco"); u32(&b, 0); u32(&b, 0); box_close(&b, x);
    box_close(&b, stbl);
    box_close(&b, minf);
    box_close(&b, mdia);
    box_close(&b, trak);

    size_t mvex = box_open(&b, "mvex");
    size_t trex = box_open(&b, "trex");
    u32(&b, 0);
    u32(&b, 1);                         /* track id */
    u32(&b, 1);                         /* default sample description */
    u32(&b, 0); u32(&b, 0); u32(&b, 0);
    box_close(&b, trex);
    box_close(&b, mvex);

    box_close(&b, moov);

    if (b.oom) {
        free(b.p);
        return NULL;
    }
    *len = b.len;
    return b.p;
}

/* Length-prefix the picture NALs of an access unit (SPS/PPS dropped:
 * they live in the init segment; AUD too, MSE does not need it). */
struct payload {
    struct bb *b;
};

static void payload_cb(void *ctx, const uint8_t *nal, size_t n)
{
    struct payload *p = ctx;
    int type = nal[0] & 0x1F;
    if (type == 7 || type == 8 || type == 9)
        return;
    u32(p->b, (uint32_t)n);
    bb_put(p->b, nal, n);
}

uint8_t *mp4mux_fragment(mp4mux_t *m, uint32_t seq, uint64_t pts,
                         uint32_t dur, int key, const uint8_t *au,
                         size_t len, size_t *outlen)
{
    (void)m;
    struct bb payload = {0};
    struct payload pctx = { &payload };
    each_nal(au, len, payload_cb, &pctx);
    if (payload.oom || payload.len == 0) {
        free(payload.p);
        return NULL;
    }

    struct bb b = {0};
    size_t moof = box_open(&b, "moof");
    size_t mfhd = box_open(&b, "mfhd");
    u32(&b, 0);
    u32(&b, seq);
    box_close(&b, mfhd);

    size_t traf = box_open(&b, "traf");
    size_t tfhd = box_open(&b, "tfhd");
    u32(&b, 0x020000);                  /* default-base-is-moof */
    u32(&b, 1);                         /* track id */
    box_close(&b, tfhd);
    size_t tfdt = box_open(&b, "tfdt");
    u8(&b, 1);                          /* version 1: 64-bit time */
    u8(&b, 0); u16(&b, 0);
    u64f(&b, pts);
    box_close(&b, tfdt);
    size_t trun = box_open(&b, "trun");
    u32(&b, 0x000701);          /* data offset, duration, size, flags */
    u32(&b, 1);                 /* one sample */
    size_t doff_at = b.len;
    u32(&b, 0);                 /* data offset, patched below */
    u32(&b, dur);
    u32(&b, (uint32_t)payload.len);
    u32(&b, key ? 0x02000000 : 0x01010000);
    box_close(&b, trun);
    box_close(&b, traf);
    box_close(&b, moof);

    /* Sample data starts just past the mdat header. */
    if (!b.oom) {
        uint32_t doff = (uint32_t)(b.len + 8);
        b.p[doff_at] = (uint8_t)(doff >> 24);
        b.p[doff_at + 1] = (uint8_t)(doff >> 16);
        b.p[doff_at + 2] = (uint8_t)(doff >> 8);
        b.p[doff_at + 3] = (uint8_t)doff;
    }

    size_t mdat = box_open(&b, "mdat");
    bb_put(&b, payload.p, payload.len);
    box_close(&b, mdat);
    free(payload.p);

    if (b.oom) {
        free(b.p);
        return NULL;
    }
    *outlen = b.len;
    return b.p;
}

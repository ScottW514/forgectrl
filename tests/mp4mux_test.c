/*
 * mp4mux_test.c - host unit test for the fragmented-MP4 muxer
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The muxer feeds browsers through MSE, which rejects a malformed box
 * tree silently, so the structure is checked here: box sizes must tile
 * their container exactly, the avcC must carry the fed SPS/PPS, the
 * trun's data offset must land on the mdat payload, and the Annex B
 * reframing must drop parameter sets and length-prefix picture NALs.
 */
#define _GNU_SOURCE     /* memmem */
#include "../src/mp4mux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
        } \
    } while (0)

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Find a box by walking a container level; returns NULL if absent. */
static const uint8_t *find_box(const uint8_t *p, size_t len,
                               const char *tag, size_t *box_len)
{
    size_t i = 0;
    while (i + 8 <= len) {
        uint32_t sz = rd32(p + i);
        if (sz < 8 || i + sz > len)
            return NULL;
        if (!memcmp(p + i + 4, tag, 4)) {
            *box_len = sz;
            return p + i;
        }
        i += sz;
    }
    return NULL;
}

/* A miniature but plausible SPS (profile 66 / constrained / level 30)
 * and PPS; the muxer treats their bodies as opaque. */
static const uint8_t SPS[] = { 0x67, 0x42, 0xC0, 0x1E, 0xD9, 0x00 };
static const uint8_t PPS[] = { 0x68, 0xCE, 0x3C, 0x80 };

int main(void)
{
    /* One Annex B access unit: SPS, PPS (4-byte start codes), then an
     * IDR slice with a 3-byte start code, exercising both framings. */
    uint8_t au[64];
    size_t n = 0;
    static const uint8_t sc4[] = { 0, 0, 0, 1 };
    static const uint8_t sc3[] = { 0, 0, 1 };
    static const uint8_t idr[] = { 0x65, 0x88, 0x84, 0x00, 0x33, 0xFF };
    memcpy(au + n, sc4, 4); n += 4;
    memcpy(au + n, SPS, sizeof(SPS)); n += sizeof(SPS);
    memcpy(au + n, sc4, 4); n += 4;
    memcpy(au + n, PPS, sizeof(PPS)); n += sizeof(PPS);
    memcpy(au + n, sc3, 3); n += 3;
    memcpy(au + n, idr, sizeof(idr)); n += sizeof(idr);

    mp4mux_t *m = mp4mux_new(1296, 972);
    CHECK(m != NULL, "mp4mux_new");
    CHECK(mp4mux_init_segment(m, &(size_t){0}) == NULL,
          "init segment produced before any parameter sets");
    CHECK(mp4mux_feed_params(m, au, n) == 1, "SPS/PPS not captured");
    CHECK(strcmp(mp4mux_codec(m), "avc1.42c01e") == 0,
          "codec string is %s", mp4mux_codec(m));

    /* Parameter re-emission round-trips. */
    uint8_t params[128];
    size_t plen = mp4mux_params_annexb(m, params, sizeof(params));
    CHECK(plen == 8 + sizeof(SPS) + sizeof(PPS), "params length %zu", plen);
    mp4mux_t *m2 = mp4mux_new(1296, 972);
    CHECK(mp4mux_feed_params(m2, params, plen) == 1,
          "re-emitted params do not parse");
    mp4mux_free(m2);

    /* Init segment structure. */
    size_t init_len = 0;
    uint8_t *init = mp4mux_init_segment(m, &init_len);
    CHECK(init != NULL && init_len > 0, "init segment");
    size_t bl;
    const uint8_t *ftyp = find_box(init, init_len, "ftyp", &bl);
    CHECK(ftyp == init, "ftyp is not first");
    const uint8_t *moov = find_box(init, init_len, "moov", &bl);
    CHECK(moov != NULL, "no moov");
    CHECK((size_t)(moov - init) + bl == init_len,
          "boxes do not tile the init segment");
    size_t moov_len = bl;
    const uint8_t *trak = find_box(moov + 8, moov_len - 8, "trak", &bl);
    CHECK(trak != NULL, "no trak");
    const uint8_t *mvex = find_box(moov + 8, moov_len - 8, "mvex", &bl);
    CHECK(mvex != NULL, "no mvex (fragmented streaming not announced)");
    /* The avcC must embed the SPS and PPS verbatim. */
    const uint8_t *avcc = NULL;
    for (size_t i = 0; i + 4 <= init_len; i++)
        if (!memcmp(init + i, "avcC", 4)) {
            avcc = init + i - 4;
            break;
        }
    CHECK(avcc != NULL, "no avcC");
    if (avcc) {
        size_t alen = rd32(avcc);
        CHECK(memmem(avcc, alen, SPS, sizeof(SPS)) != NULL,
              "SPS not in avcC");
        CHECK(memmem(avcc, alen, PPS, sizeof(PPS)) != NULL,
              "PPS not in avcC");
        CHECK(avcc[8] == 1, "avcC configuration version");
        CHECK(avcc[9] == 0x42, "avcC profile byte");
    }

    /* Fragment structure. */
    size_t frag_len = 0;
    uint8_t *frag = mp4mux_fragment(m, 1, 90000, 6000, 1, au, n,
                                    &frag_len);
    CHECK(frag != NULL, "fragment");
    const uint8_t *moof = find_box(frag, frag_len, "moof", &bl);
    CHECK(moof == frag, "moof is not first");
    size_t moof_len = bl;
    const uint8_t *mdat = find_box(frag, frag_len, "mdat", &bl);
    CHECK(mdat == frag + moof_len, "mdat does not follow moof");
    CHECK(moof_len + bl == frag_len, "boxes do not tile the fragment");
    /* The payload must be the length-prefixed IDR alone: SPS, PPS
     * dropped. */
    size_t payload = bl - 8;
    CHECK(payload == 4 + sizeof(idr), "payload is %zu bytes", payload);
    CHECK(rd32(mdat + 8) == sizeof(idr), "NAL length prefix");
    CHECK(!memcmp(mdat + 12, idr, sizeof(idr)), "IDR body");
    /* The trun data offset points at the payload. */
    const uint8_t *traf = find_box(moof + 8, moof_len - 8, "traf", &bl);
    CHECK(traf != NULL, "no traf");
    const uint8_t *trun = traf ? find_box(traf + 8, bl - 8, "trun", &bl)
                               : NULL;
    CHECK(trun != NULL, "no trun");
    if (trun) {
        CHECK(rd32(trun + 8) == 0x000701, "trun flags");
        CHECK(rd32(trun + 12) == 1, "trun sample count");
        uint32_t doff = rd32(trun + 16);
        CHECK(frag + doff == mdat + 8, "trun data offset %u", doff);
        CHECK(rd32(trun + 20) == 6000, "sample duration");
        CHECK(rd32(trun + 24) == payload, "sample size");
        CHECK(rd32(trun + 28) == 0x02000000, "key sample flags");
    }
    /* A non-key fragment marks the sample as depending on others. */
    uint8_t nonkey[32];
    size_t kn = 0;
    static const uint8_t p_slice[] = { 0x41, 0x9A, 0x02 };
    memcpy(nonkey + kn, sc4, 4); kn += 4;
    memcpy(nonkey + kn, p_slice, sizeof(p_slice)); kn += sizeof(p_slice);
    size_t f2_len = 0;
    uint8_t *f2 = mp4mux_fragment(m, 2, 96000, 6000, 0, nonkey, kn,
                                  &f2_len);
    CHECK(f2 != NULL, "non-key fragment");
    const uint8_t *moof2 = find_box(f2, f2_len, "moof", &bl);
    const uint8_t *traf2 = moof2 ? find_box(moof2 + 8, bl - 8, "traf", &bl)
                                 : NULL;
    const uint8_t *trun2 = traf2 ? find_box(traf2 + 8, bl - 8, "trun", &bl)
                                 : NULL;
    CHECK(trun2 != NULL && rd32(trun2 + 28) == 0x01010000,
          "non-key sample flags");

    /* An access unit with nothing but parameter sets yields no fragment. */
    uint8_t only_ps[32];
    size_t on = 0;
    memcpy(only_ps + on, sc4, 4); on += 4;
    memcpy(only_ps + on, SPS, sizeof(SPS)); on += sizeof(SPS);
    CHECK(mp4mux_fragment(m, 3, 0, 6000, 0, only_ps, on, &(size_t){0})
          == NULL, "parameter-set-only unit produced a fragment");

    free(init);
    free(frag);
    free(f2);
    mp4mux_free(m);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mp4mux_test: all checks passed\n");
    return 0;
}

/*
 * debayer_test.c - host unit test for the raw-frame front end
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Every demosaic in debayer.c takes 8-bit samples, so a sensor profile that
 * captures 10-bit samples in 16-bit words needs one narrowing pass between
 * the capture buffer and the whole tested pipeline. No shipped profile does
 * today - both sensors are read as 8-bit - but the OV8856's 10-bit modes are
 * the fallback if its RAW8 full-resolution mode will not lock on real
 * hardware, so the pass stays covered. It has to read little-endian
 * regardless of host byte order, saturate rather than wrap if the samples
 * turn out not to be right-aligned, and report the peak so that alignment is
 * visible in the log. The Bayer phase must survive it: a BGGR frame narrowed
 * pixel-by-pixel is still BGGR, which is checked by demosaicing a synthetic
 * frame and reading the colors back.
 */
#define _GNU_SOURCE
#include "../src/debayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static void put16(uint8_t *p, size_t i, unsigned v)
{
    p[2 * i] = (uint8_t)(v & 0xff);
    p[2 * i + 1] = (uint8_t)(v >> 8);
}

static void test_narrow(void)
{
    uint8_t src[2 * 8];
    uint8_t dst[8];
    /* 10-bit range plus the two ends of the 16-bit word */
    const unsigned in[8] = { 0, 1, 3, 4, 512, 1020, 1023, 0xffff };

    for (size_t i = 0; i < 8; i++)
        put16(src, i, in[i]);

    uint16_t peak = debayer_narrow16(src, dst, 8, 2);
    CHECK(peak == 0xffff, "peak is the largest pre-shift sample");
    CHECK(dst[0] == 0 && dst[1] == 0 && dst[2] == 0 && dst[3] == 1,
          "10-bit samples shift down by 2");
    CHECK(dst[4] == 128 && dst[5] == 255 && dst[6] == 255,
          "full-scale 10-bit maps to full-scale 8-bit");
    CHECK(dst[7] == 255, "an out-of-range sample saturates, it does not wrap");

    /* Byte order is the frame's, not the host's. */
    put16(src, 0, 0x0100);
    CHECK(debayer_narrow16(src, dst, 1, 0) == 0x0100 && dst[0] == 255,
          "samples are read little-endian");

    /* shift 0 is the identity on the low byte of an in-range sample */
    for (size_t i = 0; i < 8; i++)
        put16(src, i, i * 30);
    debayer_narrow16(src, dst, 8, 0);
    int ok = 1;
    for (size_t i = 0; i < 8; i++)
        ok = ok && dst[i] == (uint8_t)(i * 30);
    CHECK(ok, "shift 0 passes samples through");
}

/* A 6x6 BGGR frame lit only on the B sites (1023; R and G sites 0).
 * Narrowed and demosaiced it must come back blue - i.e. the narrowing did
 * not shift the Bayer phase. Interior pixels only: the demosaic clamps at
 * the border, which mixes a site with itself. */
static void test_phase_preserved(void)
{
    const int w = 6, h = 6;
    uint8_t src16[2 * 36];
    uint8_t raw8[36];
    uint8_t rgb[36 * 3];

    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned v = ((y & 1) == 0 && (x & 1) == 0) ? 1023 : 0;
            put16(src16, (size_t)y * w + x, v);
        }

    debayer_narrow16(src16, raw8, (size_t)w * h, 2);
    debayer_bggr_bilinear(raw8, rgb, w, h, 0);

    /* (2,2) is an interior B site: blue saturated, nothing else */
    const uint8_t *b = rgb + ((size_t)2 * w + 2) * 3;
    CHECK(b[2] == 255, "B site stays blue after narrowing");
    CHECK(b[0] == 0 && b[1] == 0, "B site has no red or green");

    /* (3,3) is an interior R site: no red in a blue-only frame, and blue
     * interpolated from its four diagonal B neighbors */
    const uint8_t *r = rgb + ((size_t)3 * w + 3) * 3;
    CHECK(r[0] == 0, "R site is dark in a blue-only frame");
    CHECK(r[2] == 255, "R site interpolates blue from its B neighbors");

    /* hflip mirrors the row: the B site at x=2 lands at x = w-1-2 */
    debayer_bggr_bilinear(raw8, rgb, w, h, 1);
    CHECK(rgb[((size_t)2 * w + (w - 1 - 2)) * 3 + 2] == 255,
          "hflip mirrors the row");
}

int main(void)
{
    printf("narrow16:\n");
    test_narrow();
    printf("bayer phase:\n");
    test_phase_preserved();
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

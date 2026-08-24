/*
 * mp4mux.h - fragmented MP4 wrapping for the H.264 stream
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Wraps CODA960 Annex B access units as the fragmented MP4 a browser's
 * Media Source Extensions accept: one init segment (ftyp + moov, built
 * from the SPS/PPS), then one moof + mdat pair per frame. Every
 * fragment is self-contained, so a viewer can join at any IDR. Pure
 * byte-shuffling, no codec knowledge beyond NAL framing; covered by a
 * host unit test.
 */
#ifndef FORGECTRL_MP4MUX_H
#define FORGECTRL_MP4MUX_H

#include <stddef.h>
#include <stdint.h>

typedef struct mp4mux mp4mux_t;

/* Timescale of every timestamp and duration below. */
#define MP4MUX_TIMESCALE 90000u

mp4mux_t *mp4mux_new(int w, int h);
void mp4mux_free(mp4mux_t *m);

/* Feed one Annex B access unit so the muxer can capture SPS and PPS.
 * Returns 1 once both have been seen (mp4mux_init_segment will work),
 * else 0. Safe to call on every unit; later parameter sets replace
 * earlier ones. */
int mp4mux_feed_params(mp4mux_t *m, const uint8_t *au, size_t len);

/* RFC 6381 codec string for the captured SPS ("avc1.42e01e"), for the
 * MSE addSourceBuffer MIME type. Valid after mp4mux_feed_params returned
 * 1; the buffer lives in the muxer. */
const char *mp4mux_codec(mp4mux_t *m);

/* The captured SPS and PPS re-emitted as Annex B (start code + NAL
 * each), so an engine can hand them to viewers that join after the
 * encoder's first unit. Returns the length written, 0 if they have not
 * both been captured or cap is too small. */
size_t mp4mux_params_annexb(mp4mux_t *m, uint8_t *dst, size_t cap);

/* ftyp + moov built from the captured SPS/PPS. Malloc'd (caller frees);
 * NULL before both parameter sets have been seen. */
uint8_t *mp4mux_init_segment(mp4mux_t *m, size_t *len);

/* One moof + mdat for one access unit. seq numbers fragments from 1 per
 * viewer; pts and dur are in MP4MUX_TIMESCALE units; key marks an IDR.
 * SPS/PPS NALs inside the unit are dropped (they live in the init
 * segment) and the rest converted to length-prefixed framing. Malloc'd
 * (caller frees); NULL on OOM or if the unit holds no picture data. */
uint8_t *mp4mux_fragment(mp4mux_t *m, uint32_t seq, uint64_t pts,
                         uint32_t dur, int key, const uint8_t *au,
                         size_t len, size_t *outlen);

#endif

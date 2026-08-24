/*
 * gpu_debayer.h - Bayer demosaic on the i.MX6 GC880 GPU (etnaviv/Mesa)
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Renders the superpixel half-resolution BGGR -> YUV420 conversion (the
 * same semantics as debayer_bggr_half_yuv420) as GLES2 fragment-shader
 * passes, reading the raw frame and writing the encoder's planar YUV420
 * buffer through dmabufs, so the CPU never touches frame data.
 *
 * The GL stack is loaded with dlopen at runtime (libEGL.so.1 /
 * libGLESv2.so.2): the daemon builds and runs on hosts and images with no
 * GL libraries, where gpu_debayer_open simply fails and the caller stays
 * on the NEON path. Every capability the path depends on (surfaceless
 * EGL, dmabuf import, rendering into an imported linear buffer, texture
 * size) is probed at open, never assumed.
 *
 * Threading: one instance, one thread (the camera worker).
 */
#ifndef FORGECTRL_GPU_DEBAYER_H
#define FORGECTRL_GPU_DEBAYER_H

#include <stddef.h>
#include <stdint.h>

typedef struct gpu_debayer gpu_debayer_t;

/* Bring up EGL + GLES2 and compile the conversion programs for a raw
 * frame of raw_w x raw_h (BGGR, 1 byte per sample). Returns NULL with the
 * reason logged if any piece is missing; the caller then uses the CPU
 * path. raw_w must be a multiple of 16 and raw_h a multiple of 4 (both
 * sensor modes satisfy this). */
gpu_debayer_t *gpu_debayer_open(int raw_w, int raw_h, int hflip);

void gpu_debayer_close(gpu_debayer_t *g);

/* Import one raw capture buffer (dmabuf) under slot `idx` (0..7). The fd
 * is duplicated by the EGL import; the caller keeps ownership of its
 * copy. Returns 0, or -1 with the reason logged. */
int gpu_debayer_attach_raw(gpu_debayer_t *g, int idx, int fd);

/* Import a planar YUV420 destination (dmabuf) under slot `slot` (0..1):
 * an encoder OUTPUT buffer of the half-resolution geometry, Y plane of
 * y_stride bytes per row at offset 0, then U and V planes of
 * y_stride / 2 bytes per row. Returns 0, or -1 with the reason logged. */
int gpu_debayer_attach_dst(gpu_debayer_t *g, int slot, int fd,
                           int y_stride, size_t buf_len);

/* Convert raw slot `idx` into destination slot `slot`. Blocks until the
 * GPU is done (the destination is safe to hand to the encoder on
 * return). Returns 0, or -1 with the reason logged; after a failure the
 * instance is dead and the caller falls back to the CPU path. */
int gpu_debayer_convert(gpu_debayer_t *g, int idx, int slot);

#endif

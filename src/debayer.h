/*
 * debayer.h - BGGR raw-Bayer to RGB conversion for the Glowforge cameras
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef FORGECTRL_DEBAYER_H
#define FORGECTRL_DEBAYER_H

#include <stddef.h>
#include <stdint.h>

/* Narrow a frame of 16-bit raw samples to the 8-bit samples every demosaic
 * below takes. `shift` is the right shift from sample to 8-bit value (2 for
 * a 10-bit sensor); samples are read as little-endian 16-bit words, which is
 * how the IPU CSI writes V4L2_PIX_FMT_S*16 passthrough frames, and the
 * result is clamped so an unexpectedly aligned source saturates instead of
 * wrapping. Returns the largest sample seen before the shift - a caller can
 * log it to confirm the sample alignment on a sensor it has not measured.
 * `n` is the pixel count; `src` holds 2*n bytes and `dst` n bytes. */
uint16_t debayer_narrow16(const uint8_t *src, uint8_t *dst, size_t n,
                          int shift);

/* Full-resolution bilinear demosaic of a BGGR frame. rgb must hold w*h*3
 * bytes. hflip mirrors the output horizontally (the factory image
 * orientation: the sensor HFLIP register breaks imx-media CSI capture, so
 * the mirror is applied in software). */
void debayer_bggr_bilinear(const uint8_t *raw, uint8_t *rgb,
                           int w, int h, int hflip);

/* Half-resolution demosaic: each 2x2 BGGR quad becomes one RGB pixel
 * (greens averaged) - no interpolation. Output is (w/2)x(h/2); rgb must
 * hold (w/2)*(h/2)*3 bytes. */
void debayer_bggr_half(const uint8_t *raw, uint8_t *rgb,
                       int w, int h, int hflip);

/* Half-resolution demosaic straight to planar YUV420 (JFIF full-range,
 * ITU-R 601) for the VPU JPEG encoder: luma per 2x2 BGGR quad at
 * (w/2)x(h/2), chroma averaged per 2x2 luma block at (w/4)x(h/4).
 * w/2 and h/2 must be even. Strides are in bytes.
 *
 * Dispatches to a NEON kernel when compiled for NEON and the geometry
 * allows (w%32==0, h%4==0); FORGECTRL_NO_NEON=1 forces the scalar path.
 * Both paths produce bit-identical output. */
void debayer_bggr_half_yuv420(const uint8_t *raw, int w, int h, int hflip,
                              uint8_t *yp, int y_stride,
                              uint8_t *up, uint8_t *vp, int uv_stride);

/* The scalar reference path (used by the NEON self-check). */
void debayer_bggr_half_yuv420_scalar(const uint8_t *raw, int w, int h,
                                     int hflip, uint8_t *yp, int y_stride,
                                     uint8_t *up, uint8_t *vp,
                                     int uv_stride);

#endif

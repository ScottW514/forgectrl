/*
 * vpu_h264.h - hardware H.264 encoding on the i.MX6 CODA960 VPU
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Thin wrapper around the mainline coda V4L2 mem2mem H.264 encoder (the
 * BIT processor, a separate engine from the JPEG unit vpu_jpeg.h uses,
 * so both can be open at once). The caller fills planar YUV420 into the
 * OUTPUT buffer, directly or through its dmabuf, and gets back one
 * Annex B access unit per frame; the first unit of a stream carries the
 * SPS and PPS.
 */
#ifndef FORGECTRL_VPU_H264_H
#define FORGECTRL_VPU_H264_H

#include <stddef.h>
#include <stdint.h>

typedef struct vpu_h264 vpu_h264_t;

/* Locate the CODA H.264 encoder node and configure it for w x h YUV420
 * at fps frames per second (rate-control input), the given bitrate (bits
 * per second) and GOP length (frames per IDR). w must be even and at
 * most 1920, h even and at most 1088 (the CODA960 encode ceiling).
 * Returns NULL if no encoder exists or setup fails. */
vpu_h264_t *vpu_h264_open(int w, int h, int fps, int bitrate, int gop);

/* Planes of the mapped OUTPUT buffer for direct CPU fill. */
void vpu_h264_planes(vpu_h264_t *v, uint8_t **y, uint8_t **u, uint8_t **vv,
                     int *y_stride, int *uv_stride);

/* The OUTPUT buffer as a dmabuf for a GPU fill; owned by the encoder.
 * Returns the fd, or -1 if the queue cannot export. */
int vpu_h264_out_dmabuf(vpu_h264_t *v, int *stride, size_t *len);

/* Make the next encoded frame an IDR (a joining viewer needs one). */
void vpu_h264_force_key(vpu_h264_t *v);

/* Encode the currently-filled OUTPUT buffer. On success *au is malloc'd
 * Annex B bytes (caller frees), *key says whether it contains an IDR.
 * Returns 0; 1 for a dropped frame (transient, keep the encoder); -1 for
 * a hard failure. */
int vpu_h264_encode(vpu_h264_t *v, uint8_t **au, size_t *len, int *key);

void vpu_h264_close(vpu_h264_t *v);

#endif

/*
 * ipu_copy.h - IPU stride-fix crop between the GPU and the encoders
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The GC880 can only write byte-exact rows padded to 64-byte multiples,
 * and the CODA fixes its luma stride to round_up(width, 16); for our
 * stream widths the two never meet. The IPU's mem2mem CSC/scaler closes
 * the gap in hardware: the GPU renders into this module's wider source
 * buffer (whose stride the GPU can hit exactly) and the IPU crops the
 * picture into the encoder's own buffer over dmabuf, so the frame still
 * never crosses the CPU.
 *
 * One instance holds one m2m context: source geometry src_w x h YUV420
 * cropped to w x h. The destination changes per run (JPEG or H.264
 * encoder OUTPUT buffer), which V4L2 dmabuf queueing permits.
 */
#ifndef FORGECTRL_IPU_COPY_H
#define FORGECTRL_IPU_COPY_H

#include <stddef.h>
#include <stdint.h>

typedef struct ipu_copy ipu_copy_t;

/* The source stride (and width) the GPU needs for a stream width w:
 * both the luma and the half-width chroma rows must land on the GPU's
 * 64-byte render boundary. */
static inline int ipu_copy_src_width(int w)
{
    return (w + 127) & ~127;
}

/* Find the imx-csc-scaler node and configure it: source src_w x h
 * YUV420 (its own MMAP buffer), cropped to the top-left w x h, into a
 * caller-supplied dmabuf per run. Returns NULL if the device is absent
 * or refuses the geometry, with the reason logged. */
ipu_copy_t *ipu_copy_open(int src_w, int w, int h);

/* The source buffer as a dmabuf for the GPU to render into; owned by
 * this module. *stride is the luma stride (= src_w), *len the buffer
 * length. */
int ipu_copy_src_dmabuf(ipu_copy_t *c, int *stride, size_t *len);

/* Crop the current source content into dst_fd (a YUV420 buffer of the
 * cropped geometry, dst_len bytes - an encoder OUTPUT buffer). Blocks
 * until the IPU is done. Returns 0, or -1 with the reason logged. */
int ipu_copy_run(ipu_copy_t *c, int dst_fd, size_t dst_len);

void ipu_copy_close(ipu_copy_t *c);

#endif

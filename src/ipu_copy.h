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

/* Sources the module keeps: two, so a frame can render into one while
 * the previous frame's copies run from the other. */
#define IPU_COPY_SRCS 2

/* Find the imx-csc-scaler node and configure it: IPU_COPY_SRCS source
 * buffers of src_w x h YUV420 (its own MMAP buffers), each cropped to
 * the top-left w x h into a caller-supplied dmabuf per run. Returns
 * NULL if the device is absent or refuses the geometry, with the
 * reason logged. */
ipu_copy_t *ipu_copy_open(int src_w, int w, int h);

/* Source buffer `i` (0..IPU_COPY_SRCS-1) as a dmabuf for the GPU to
 * render into; owned by this module. *stride is the luma stride
 * (= src_w), *len the buffer length. */
int ipu_copy_src_dmabuf(ipu_copy_t *c, int i, int *stride, size_t *len);

/* Crop source buffer `i` into dst_fd (a YUV420 buffer of the cropped
 * geometry, dst_len bytes - an encoder OUTPUT buffer). Blocks until
 * the IPU is done. Returns 0, or -1 with the reason logged. */
int ipu_copy_run(ipu_copy_t *c, int i, int dst_fd, size_t dst_len);

/* CPU view of source buffer `i` (mapped on first use; slow uncached
 * reads) - a bench diagnostic for comparing the GPU's output before the
 * IPU touched it. NULL if the mapping fails. */
const uint8_t *ipu_copy_src_map(ipu_copy_t *c, int i);

void ipu_copy_close(ipu_copy_t *c);

#endif

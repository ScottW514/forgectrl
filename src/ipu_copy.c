/*
 * ipu_copy.c - IPU stride-fix crop between the GPU and the encoders
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * V4L2 mem2mem against the mainline imx-csc-scaler (the IPU IC's
 * post-processing task): one MMAP buffer on the OUTPUT (source) queue,
 * exported as the GPU's render target, and a caller dmabuf on the
 * CAPTURE queue each run. Both sides are YUV420 at the same height and
 * colorimetry, so the IC copies rather than converts; the source crop
 * rectangle drops the GPU's alignment columns. The node is found by
 * driver name, never by number.
 */
#include "ipu_copy.h"
#include "fflog.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define COPY_TIMEOUT_MS 1000

struct ipu_copy {
    int    fd;
    int    src_w, w, h;
    int    src_dmabuf;
    size_t src_len;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static int is_csc_scaler(int fd)
{
    struct v4l2_capability cap = {0};
    return xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
           strcmp((const char *)cap.driver, "imx-csc-scaler") == 0 &&
           (cap.device_caps & V4L2_CAP_VIDEO_M2M);
}

static int find_scaler(void)
{
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
        if (fd < 0)
            continue;
        if (is_csc_scaler(fd))
            return fd;
        close(fd);
    }
    return -1;
}

static int set_fmt(int fd, enum v4l2_buf_type type, int w, int h)
{
    struct v4l2_format f = { .type = type };
    f.fmt.pix.width = (unsigned)w;
    f.fmt.pix.height = (unsigned)h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0 ||
        f.fmt.pix.width != (unsigned)w || f.fmt.pix.height != (unsigned)h ||
        f.fmt.pix.bytesperline != (unsigned)w) {
        fflog(LOG_INFO, "ipu: S_FMT %dx%d refused (got %ux%u stride %u)",
              w, h, f.fmt.pix.width, f.fmt.pix.height,
              f.fmt.pix.bytesperline);
        return -1;
    }
    return 0;
}

ipu_copy_t *ipu_copy_open(int src_w, int w, int h)
{
    ipu_copy_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->src_dmabuf = -1;
    c->src_w = src_w;
    c->w = w;
    c->h = h;
    c->fd = find_scaler();
    if (c->fd < 0) {
        fflog(LOG_INFO, "ipu: no csc-scaler node");
        goto fail;
    }

    if (set_fmt(c->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, src_w, h) ||
        set_fmt(c->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, w, h))
        goto fail;

    struct v4l2_selection sel = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .target = V4L2_SEL_TGT_CROP,
        .r = { .left = 0, .top = 0,
               .width = (unsigned)w, .height = (unsigned)h },
    };
    if (xioctl(c->fd, VIDIOC_S_SELECTION, &sel) < 0 ||
        sel.r.width != (unsigned)w || sel.r.height != (unsigned)h ||
        sel.r.left != 0 || sel.r.top != 0) {
        fflog(LOG_INFO, "ipu: source crop %dx%d refused", w, h);
        goto fail;
    }

    struct v4l2_requestbuffers req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
        fflog(LOG_INFO, "ipu: source REQBUFS refused: %s", strerror(errno));
        goto fail;
    }
    struct v4l2_buffer buf = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                               .memory = V4L2_MEMORY_MMAP, .index = 0 };
    if (xioctl(c->fd, VIDIOC_QUERYBUF, &buf) < 0)
        goto fail;
    c->src_len = buf.length;

    struct v4l2_exportbuffer exp = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .index = 0,
        .flags = O_CLOEXEC,
    };
    if (xioctl(c->fd, VIDIOC_EXPBUF, &exp) < 0) {
        fflog(LOG_INFO, "ipu: source dmabuf export refused: %s",
              strerror(errno));
        goto fail;
    }
    c->src_dmabuf = exp.fd;

    struct v4l2_requestbuffers creq = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_DMABUF,
    };
    if (xioctl(c->fd, VIDIOC_REQBUFS, &creq) < 0 || creq.count < 1) {
        fflog(LOG_INFO, "ipu: dmabuf destination REQBUFS refused: %s",
              strerror(errno));
        goto fail;
    }

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(c->fd, VIDIOC_STREAMON, &t) < 0)
        goto fail;
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &t) < 0)
        goto fail;

    fflog(LOG_INFO, "ipu: stride-fix crop up, %dx%d -> %dx%d",
          src_w, h, w, h);
    return c;

fail:
    ipu_copy_close(c);
    return NULL;
}

int ipu_copy_src_dmabuf(ipu_copy_t *c, int *stride, size_t *len)
{
    *stride = c->src_w;
    *len = c->src_len;
    return c->src_dmabuf;
}

int ipu_copy_run(ipu_copy_t *c, int dst_fd, size_t dst_len)
{
    struct v4l2_buffer db = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                              .memory = V4L2_MEMORY_DMABUF, .index = 0 };
    db.m.fd = dst_fd;
    db.length = (unsigned)dst_len;
    struct v4l2_buffer sb = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                              .memory = V4L2_MEMORY_MMAP, .index = 0 };
    sb.bytesused = (unsigned)c->src_len;

    if (xioctl(c->fd, VIDIOC_QBUF, &db) < 0 ||
        xioctl(c->fd, VIDIOC_QBUF, &sb) < 0) {
        fflog(LOG_WARNING, "ipu: copy QBUF failed: %s", strerror(errno));
        return -1;
    }
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    int pr;
    do {
        pr = poll(&pfd, 1, COPY_TIMEOUT_MS);
    } while (pr == -1 && errno == EINTR);
    if (pr <= 0) {
        fflog(LOG_WARNING, "ipu: copy timed out");
        return -1;
    }
    if (xioctl(c->fd, VIDIOC_DQBUF, &db) < 0) {
        fflog(LOG_WARNING, "ipu: copy DQBUF failed: %s", strerror(errno));
        return -1;
    }
    xioctl(c->fd, VIDIOC_DQBUF, &sb);
    return (db.flags & V4L2_BUF_FLAG_ERROR) ? -1 : 0;
}

void ipu_copy_close(ipu_copy_t *c)
{
    if (!c)
        return;
    if (c->fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        xioctl(c->fd, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(c->fd, VIDIOC_STREAMOFF, &t);
    }
    if (c->src_dmabuf >= 0)
        close(c->src_dmabuf);
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}

/*
 * vpu_h264.c - hardware H.264 encoding on the i.MX6 CODA960 VPU
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * V4L2 mem2mem against the mainline coda driver, one MMAP buffer per
 * queue, synchronous QBUF/DQBUF per frame - the same shape as the JPEG
 * wrapper (vpu_jpeg.c), against the other encoder personality: the node
 * whose capture side offers H264 (the BIT processor; the JPEG unit is a
 * separate engine, so both wrappers can hold their nodes at once).
 * The node is found by personality, never by number.
 */
#include "vpu_h264.h"
#include "fflog.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define ENCODE_TIMEOUT_MS 1000

struct vpu_h264 {
    int      fd;
    int      w, h;
    int      bpl;           /* OUTPUT luma stride from S_FMT */
    uint8_t *out;           /* mapped OUTPUT (YUV420) buffer */
    size_t   out_size;
    uint8_t *cap;           /* mapped CAPTURE (H.264) buffer */
    size_t   cap_size;
    int      out_dmabuf;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

/* Is this node the coda H.264 encoder? (H264 on the capture side, YUV420
 * accepted on the output side.) */
static int is_h264_encoder(int fd)
{
    struct v4l2_capability cap = {0};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        strcmp((const char *)cap.driver, "coda") != 0 ||
        !(cap.device_caps & V4L2_CAP_VIDEO_M2M))
        return 0;

    int h264 = 0;
    for (unsigned i = 0; ; i++) {
        struct v4l2_fmtdesc fc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                   .index = i };
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fc) < 0)
            break;
        if (fc.pixelformat == V4L2_PIX_FMT_H264)
            h264 = 1;
    }
    if (!h264)
        return 0;
    for (unsigned i = 0; ; i++) {
        struct v4l2_fmtdesc fo = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                                   .index = i };
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fo) < 0)
            return 0;
        if (fo.pixelformat == V4L2_PIX_FMT_YUV420)
            return 1;
    }
}

static int find_encoder(void)
{
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC, 0);
        if (fd < 0)
            continue;
        if (is_h264_encoder(fd))
            return fd;
        close(fd);
    }
    return -1;
}

static int map_one(int fd, enum v4l2_buf_type type, uint8_t **mem,
                   size_t *size)
{
    struct v4l2_requestbuffers req = { .count = 1, .type = type,
                                       .memory = V4L2_MEMORY_MMAP };
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
        return -1;
    struct v4l2_buffer buf = { .type = type, .memory = V4L2_MEMORY_MMAP,
                               .index = 0 };
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
        return -1;
    *mem = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                fd, buf.m.offset);
    if (*mem == MAP_FAILED) {
        *mem = NULL;
        return -1;
    }
    *size = buf.length;
    return 0;
}

static void set_ctrl(int fd, unsigned id, int value, const char *name,
                     int required, int *ok)
{
    struct v4l2_control c = { .id = id, .value = value };
    if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0) {
        fflog(required ? LOG_WARNING : LOG_DEBUG,
              "vpu: h264 %s=%d refused: %s", name, value, strerror(errno));
        if (required)
            *ok = 0;
    }
}

vpu_h264_t *vpu_h264_open(int w, int h, int fps, int bitrate, int gop)
{
    if (w < 16 || w > 1920 || h < 16 || h > 1088 || (w | h) & 1) {
        fflog(LOG_WARNING, "vpu: %dx%d is outside the CODA960 H.264 "
              "encode range", w, h);
        return NULL;
    }
    vpu_h264_t *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->out_dmabuf = -1;
    v->fd = find_encoder();
    if (v->fd < 0) {
        fflog(LOG_INFO, "vpu: no H.264 encoder node");
        goto fail;
    }

    struct v4l2_format fo = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
    fo.fmt.pix.width = (unsigned)w;
    fo.fmt.pix.height = (unsigned)h;
    fo.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fo.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(v->fd, VIDIOC_S_FMT, &fo) < 0 ||
        fo.fmt.pix.width != (unsigned)w ||
        fo.fmt.pix.height != (unsigned)h) {
        fflog(LOG_WARNING, "vpu: h264 S_FMT output rejected %dx%d (got "
              "%ux%u)", w, h, fo.fmt.pix.width, fo.fmt.pix.height);
        goto fail;
    }
    v->w = w;
    v->h = h;
    v->bpl = (int)fo.fmt.pix.bytesperline;

    struct v4l2_format fc = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    fc.fmt.pix.width = (unsigned)w;
    fc.fmt.pix.height = (unsigned)h;
    fc.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    fc.fmt.pix.sizeimage = (unsigned)(w * h);   /* far above any real AU */
    if (xioctl(v->fd, VIDIOC_S_FMT, &fc) < 0)
        goto fail;

    /* Rate control input: frame cadence on the OUTPUT queue. */
    struct v4l2_streamparm parm = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = (unsigned)(fps > 0 ? fps
                                                                   : 15);
    xioctl(v->fd, VIDIOC_S_PARM, &parm);        /* best effort */

    int ok = 1;
    set_ctrl(v->fd, V4L2_CID_MPEG_VIDEO_BITRATE, bitrate, "bitrate", 1,
             &ok);
    set_ctrl(v->fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop, "gop", 1, &ok);
    /* SPS/PPS in-band with each IDR, so any joining viewer's first
     * delivered unit is self-describing. */
    set_ctrl(v->fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
             V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME,
             "header-mode", 0, &ok);
    set_ctrl(v->fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1,
             "repeat-headers", 0, &ok);
    if (!ok)
        goto fail;

    if (map_one(v->fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, &v->out, &v->out_size) ||
        map_one(v->fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, &v->cap, &v->cap_size))
        goto fail;

    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(v->fd, VIDIOC_STREAMON, &t) < 0)
        goto fail;
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(v->fd, VIDIOC_STREAMON, &t) < 0)
        goto fail;
    fflog(LOG_INFO, "vpu: H.264 encoder up, %dx%d at %d fps, %d kbit/s, "
          "GOP %d", w, h, fps, bitrate / 1000, gop);
    return v;

fail:
    vpu_h264_close(v);
    return NULL;
}

void vpu_h264_planes(vpu_h264_t *v, uint8_t **y, uint8_t **u, uint8_t **vv,
                     int *y_stride, int *uv_stride)
{
    *y = v->out;
    *u = v->out + (size_t)v->bpl * v->h;
    *vv = *u + (size_t)(v->bpl / 2) * (v->h / 2);
    *y_stride = v->bpl;
    *uv_stride = v->bpl / 2;
}

int vpu_h264_out_dmabuf(vpu_h264_t *v, int *stride, size_t *len)
{
    if (v->out_dmabuf < 0) {
        struct v4l2_exportbuffer exp = {
            .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
            .index = 0,
            .flags = O_CLOEXEC,
        };
        if (xioctl(v->fd, VIDIOC_EXPBUF, &exp) < 0) {
            fflog(LOG_INFO, "vpu: h264 OUTPUT dmabuf export refused: %s",
                  strerror(errno));
            return -1;
        }
        v->out_dmabuf = exp.fd;
    }
    *stride = v->bpl;
    *len = v->out_size;
    return v->out_dmabuf;
}

void vpu_h264_force_key(vpu_h264_t *v)
{
    struct v4l2_control c = { .id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME };
    xioctl(v->fd, VIDIOC_S_CTRL, &c);
}

int vpu_h264_encode(vpu_h264_t *v, uint8_t **au, size_t *len, int *key)
{
    struct v4l2_buffer cb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                              .memory = V4L2_MEMORY_MMAP, .index = 0 };
    struct v4l2_buffer ob = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
                              .memory = V4L2_MEMORY_MMAP, .index = 0 };
    ob.bytesused = (unsigned)v->out_size;

    if (xioctl(v->fd, VIDIOC_QBUF, &cb) < 0 ||
        xioctl(v->fd, VIDIOC_QBUF, &ob) < 0)
        return -1;

    struct pollfd pfd = { .fd = v->fd, .events = POLLIN };
    int pr;
    do {
        pr = poll(&pfd, 1, ENCODE_TIMEOUT_MS);
    } while (pr == -1 && errno == EINTR);
    if (pr <= 0)
        return -1;

    if (xioctl(v->fd, VIDIOC_DQBUF, &cb) < 0)
        return -1;
    xioctl(v->fd, VIDIOC_DQBUF, &ob);

    if ((cb.flags & V4L2_BUF_FLAG_ERROR) || cb.bytesused == 0 ||
        cb.bytesused > v->cap_size)
        return 1;

    uint8_t *out = malloc(cb.bytesused);
    if (!out)
        return -1;
    memcpy(out, v->cap, cb.bytesused);
    *au = out;
    *len = cb.bytesused;
    *key = (cb.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
    return 0;
}

void vpu_h264_close(vpu_h264_t *v)
{
    if (!v)
        return;
    if (v->fd >= 0) {
        enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        xioctl(v->fd, VIDIOC_STREAMOFF, &t);
        t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(v->fd, VIDIOC_STREAMOFF, &t);
    }
    if (v->out_dmabuf >= 0)
        close(v->out_dmabuf);
    if (v->out)
        munmap(v->out, v->out_size);
    if (v->cap)
        munmap(v->cap, v->cap_size);
    if (v->fd >= 0)
        close(v->fd);
    free(v);
}

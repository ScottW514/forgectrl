/*
 * cam.h - persistent Glowforge camera capture engine
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One worker thread owns the imx-media pipeline and V4L2 capture node for
 * the selected camera (lid or head - they share the video-mux, so exactly
 * one can stream at a time). The engine starts on demand, publishes the
 * latest half-resolution JPEG for stream clients, serves full-resolution
 * snapshot requests from the same raw frames, and tears the pipeline down
 * after an idle period so one-shot users (gfhardware) can still grab.
 *
 * Geometry is not fixed: it comes from whichever sensor bound (5 MP OV5648
 * or 8 MP OV8856), so callers take the frame size from cam_status rather
 * than assuming one.
 *
 * Privacy: no camera captures unless the lid is closed. Every entry point
 * below refuses while the lid is open, a lid opened mid-capture stops the
 * engine, and the lid check fails closed. This is enforced here rather
 * than at the callers so it covers the panel, stream clients and the
 * cloud client alike.
 */
#ifndef FORGECTRL_CAM_H
#define FORGECTRL_CAM_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CAM_LID  = 0,
    CAM_HEAD = 1,
} cam_id_t;

/* The privacy gate's refusal, reported through the `err` buffer of every
 * entry point below. Callers that map failures to a status code match on
 * it, so it lives here rather than being spelled twice. */
#define CAM_ERR_LID "lid is open: the cameras only capture with the lid closed"

/* Read once at startup (env overrides): stream JPEG quality, lamp level,
 * stream FPS cap, and the encoder/buffer fallback switches. */
void cam_engine_init(void);

/* Stop the engine (if running) and release everything. */
void cam_engine_shutdown(void);

/* Blocking snapshot from the live engine. full=1 -> the sensor's full
 * frame, bilinear; full=0 -> half that in each axis (see cam_status for
 * the geometry: 2592x1944 / 1296x972 on a 5 MP OV5648 machine,
 * 3264x2448 / 1632x1224 on an 8 MP OV8856 one). quality 1..100. lamp
 * overrides the scene lamp for
 * this shot only (0..1023; -1 = engine default) - a few frames are
 * drained after the change so the delivered frame is exposed under the
 * requested lighting. On success *jpeg is malloc'd (caller frees). If
 * the other camera is streaming, the worker borrows the mux for one
 * frame (the stream freezes for a few seconds) - snapshots do not fail
 * busy. Returns 0, or -1 with a message in err (pipeline failure,
 * timeout). */
int cam_snapshot(cam_id_t cam, int full, int quality, int lamp,
                 uint8_t **jpeg, size_t *len, char *err, size_t errlen);

/* Stream client: open makes the engine serve `cam` (starting it, or
 * preempting the current clients and switching - last request wins; the
 * preempted clients' next() returns -1 so their streams end cleanly).
 * next blocks for a frame newer than the last one returned and copies it
 * into a client-owned buffer, close releases the pin. */
typedef struct cam_client cam_client_t;

cam_client_t *cam_client_open(cam_id_t cam, char *err, size_t errlen);
/* Returns frame length (>0), or -1 when the engine stopped / timed out and
 * the stream should end. The returned pointer stays valid until the next
 * cam_client_next() or cam_client_close(). */
long cam_client_next(cam_client_t *c, const uint8_t **jpeg);
void cam_client_close(cam_client_t *c);

/* Status snapshot for /cam/status. */
struct cam_status {
    int      running;
    cam_id_t cam;
    int      clients;
    uint64_t seq;
    double   fps;
    double   fps_cap;   /* configured stream ceiling; 0 = sensor max */
    int      vpu;       /* stream frames are VPU-encoded */
    int      cached;    /* capture buffers are CPU-cached (non-coherent) */
    /* The sensor `cam` carries and the geometry that follows from it;
     * "unknown" with zeroes if no sensor has been resolved on that bus. */
    const char *sensor;
    int      snap_w, snap_h;
    int      stream_w, stream_h;
    /* Privacy gate: lid_closed is the live lid reading (capture is
     * refused whenever it is 0), lid_stopped records that the last
     * capture ended because the lid opened rather than going idle. */
    int      lid_closed;
    int      lid_stopped;
    /* Frame health since the daemon started, across every capture: frames
     * dequeued, how many of those the capture queue flagged errored (short,
     * torn or off-sync - they are dropped, never served), and how many
     * times a run of them restarted the stream. A machine with a marginal
     * camera cable shows up here as a nonzero corrupt count. */
    uint64_t frames;
    uint64_t corrupt;
    unsigned recoveries;
};
void cam_get_status(struct cam_status *st);

const char *cam_name(cam_id_t cam);

/* The configured idle level of the lid lamp (lid_lamp_idle, default
 * 236) and its application: written now, or at the current lid
 * capture's teardown. */
int cam_lamp_idle_level(void);
void cam_lamp_apply_idle(void);

#endif

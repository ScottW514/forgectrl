/*
 * update.h - forgectrl: firmware update manager
 * Copyright (c) 2026 Scott Wiederhold <s.e.wiederhold@gmail.com>
 * SPDX-License-Identifier: MIT
 */
#ifndef UPDATE_H
#define UPDATE_H

#include <stddef.h>
#include <ulfius.h>

void update_init(void);
int  update_job_running(void);

int cb_slots(const struct _u_request *req, struct _u_response *res,
             void *user_data);
int cb_boot_select(const struct _u_request *req, struct _u_response *res,
                   void *user_data);
int cb_system_reboot(const struct _u_request *req, struct _u_response *res,
                     void *user_data);
int cb_update_check(const struct _u_request *req, struct _u_response *res,
                    void *user_data);
int cb_update_download(const struct _u_request *req, struct _u_response *res,
                       void *user_data);
int cb_update_apply(const struct _u_request *req, struct _u_response *res,
                    void *user_data);
int cb_update_upload(const struct _u_request *req, struct _u_response *res,
                     void *user_data);
int cb_restore_factory(const struct _u_request *req, struct _u_response *res,
                       void *user_data);
int cb_update_status(const struct _u_request *req, struct _u_response *res,
                     void *user_data);

/* Streaming body sink for /update/upload (ulfius upload-file callback:
 * chunks land on disk, never in RAM). */
int update_upload_sink(const struct _u_request *req, const char *key,
                       const char *filename, const char *content_type,
                       const char *transfer_encoding, const char *data,
                       uint64_t off, size_t size, void *user_data);

#endif

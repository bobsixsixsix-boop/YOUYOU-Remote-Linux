/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    const uint8_t *data;
    size_t size;
    uint64_t serial;
    int32_t width, height, format;
} YYFrame;
/* Set a private working directory before opening the core. All calls belong
   to one UI thread. Key and client ID are copied. No automatic login/network. */
void *yy_open(const char *key, const char *client_id);
void yy_close(void *core);
const char *yy_error(void);
int yy_command(void *core, const char *name, const char *json);
/* Borrowed UTF-8 JSON, valid until the next snapshot or close. */
const char *yy_snapshot(void *core);
/* 1 = new, 0 = no frame, -1 = error. Frame storage is borrowed until the next
   yy_frame or yy_close. Format 0 = RGBA, 1 = NV12 BT.601, 2 = NV12 BT.709. */
int yy_frame(void *core, uint64_t after, YYFrame *out);
/* Offline test using the bundled synthetic H.264 fixture. Zero on success. */
int yy_test_media(void);
#ifdef __cplusplus
}
#endif

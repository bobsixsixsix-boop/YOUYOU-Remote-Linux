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
typedef struct SDL_Renderer SDL_Renderer;
/* Set a private working directory before opening the core. All calls belong
   to one UI thread. Key and client ID are copied. No automatic login/network. */
void *yy_open(const char *key, const char *client_id);
void yy_close(void *core);
const char *yy_error(void);
int yy_command(void *core, const char *name, const char *json);
/* Borrowed UTF-8 JSON, valid until the next snapshot or close. */
const char *yy_snapshot(void *core);
/* 1 = new, 0 = no frame, -1 = error. Frame storage is borrowed until the next
   yy_frame or yy_close. Format 0 = RGBA, 1 = NV12 BT.601, 2 = NV12 BT.709.
   Format 3 = borrowed const AVFrame* in data, size = 0 (VAAPI only). It is an
   opaque hardware handle: never read data as pixels or upload it with SDL. */
int yy_frame(void *core, uint64_t after, YYFrame *out);
/* Experimental GLES2/EGL DMA-BUF presenter. All calls must use the renderer's
   UI thread. open returns NULL when unavailable and leaves CPU output active.
   Close the presenter before destroying the SDL renderer or closing the core. */
void *yy_gpu_open(void *core, SDL_Renderer *renderer);
void yy_gpu_close(void *presenter);
/* 1 = imported new frame, 2 = bounded GPU backlog: keep drawing the old frame,
   0 = unavailable (native output disabled; CPU fallback), -1 = invalid call.
   A successful import retains its own AVFrame reference past the next yy_frame. */
int yy_gpu_upload(void *presenter, const void *avframe);
/* 1 = drawn, 0 = no current frame/fallback, -1 = invalid call. */
int yy_gpu_draw(void *presenter, int x, int y, int width, int height);
/* Call after SDL_RenderPresent; records a fence and collects completed reads.
   1 = active, 0 = native output disabled; -1 = invalid call. */
int yy_gpu_presented(void *presenter);
/* Clear GPU frames at disconnect/resize while the SDL renderer is alive.
   Does not reset the decoder, session, or reactivate a failed presenter. */
int yy_gpu_reset(void *presenter);
/* Offline test using the bundled synthetic H.264 fixture. Zero on success. */
int yy_test_media(void);
#ifdef __cplusplus
}
#endif

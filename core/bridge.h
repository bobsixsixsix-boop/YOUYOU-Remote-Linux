/* SPDX-License-Identifier: GPL-3.0-or-later
 * C ABI of libyouyou_core, used by the Rust player. One client per process.
 * Strings are UTF-8. Calls that can fail return -1 (or NULL) and leave the
 * reason in yy_error(), valid until the next failing call on that thread. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* key: 24-byte request-signing key, client_id: 36-char device UUID (account crate). */
int yy_open(const char *key, const char *client_id);
void yy_close(void);
const char *yy_error(void);
/* Upstream Client::command: name plus a JSON argument ("{}" when it takes none). */
int yy_command(const char *name, const char *json);
/* Login/session/stream state as one JSON object; release with yy_free. */
char *yy_snapshot(void);
void yy_free(char *text);

/* One decoded picture lent out of the decoder: NV12 in one DMA-BUF, luma at
 * offset 0 and chroma at uv_offset, both rows `stride` bytes apart. */
typedef struct yy_frame {
    int fd;            /* dup'ed DMA-BUF; the caller closes it */
    uint32_t width, height;
    uint32_t stride, uv_offset;
    uint32_t index;    /* decoder buffer, for yy_frame_release */
    uint64_t epoch;    /* decoder setup the buffer belongs to */
    uint64_t serial;   /* +1 per decoded picture */
    int64_t ready_us;  /* CLOCK_MONOTONIC when it left the decoder */
} yy_frame;
/* Newest picture not taken yet (1), or none (0). Pictures nobody took in
 * time go back to the decoder: the player always gets the newest. */
int yy_frame_acquire(yy_frame *out);
/* Oldest picture not taken yet, for the frame pacer: how many were waiting,
 * this one included, or 0. Beyond three the oldest go back to the decoder. */
int yy_frame_next(yy_frame *out);
/* Give the buffer back once it is no longer on screen. */
void yy_frame_release(const yy_frame *frame);
/* Wait until a picture newer than `serial` is ready: its serial, or 0 on timeout. */
uint64_t yy_frame_wait(uint64_t serial, int timeout_ms);
/* Self-test without a session (youyou play): one Annex-B access unit per call. */
void yy_debug_video(const void *data, size_t size, int hevc);

#ifdef __cplusplus
}
#endif

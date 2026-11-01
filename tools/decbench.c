// Decode-latency bench: feeds a clip at its real frame rate and measures, per
// frame, the time from avcodec_send_packet() to the decoded picture. This is
// what decides "跟手" — throughput benchmarks (ffmpeg -benchmark) hide
// pipeline depth. For VA-API it also maps every frame to DRM PRIME, which is
// the zero-copy hand-off the player uses.
//
//   decbench <clip> <vaapi|v4l2m2m|sw> [fps]
//
// Build: cc -O2 -o decbench decbench.c $(pkg-config --cflags --libs libavformat libavcodec libavutil) -lm
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_FRAMES 100000
static double sendAt[MAX_FRAMES], lat[MAX_FRAMES], ready[MAX_FRAMES];

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static void sleep_until(double ms) {
    struct timespec t = {(time_t)(ms / 1e3), (long)(fmod(ms, 1e3) * 1e6)};
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
}
static enum AVPixelFormat pick_vaapi(AVCodecContext *c, const enum AVPixelFormat *f) {
    (void)c;
    for (; *f != AV_PIX_FMT_NONE; f++)
        if (*f == AV_PIX_FMT_VAAPI) return *f;
    fprintf(stderr, "decoder offered no VAAPI format\n");
    return AV_PIX_FMT_NONE;
}
static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <clip> <vaapi|v4l2m2m|sw> [fps]\n", argv[0]);
        return 2;
    }
    const char *mode = argv[2];
    const double fps = argc > 3 ? atof(argv[3]) : 120.0, interval = 1000.0 / fps;
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) return 1;
    const AVCodecParameters *par = fmt->streams[si]->codecpar;
    const AVCodec *codec = NULL;
    if (!strcmp(mode, "v4l2m2m")) {
        char name[32];
        snprintf(name, sizeof name, "%s_v4l2m2m", avcodec_get_name(par->codec_id));
        codec = avcodec_find_decoder_by_name(name);
    } else if (!strcmp(mode, "vaapi") && par->codec_id == AV_CODEC_ID_AV1)
        codec = avcodec_find_decoder_by_name("av1"); // libdav1d registers first and has no hwaccel
    else
        codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        fprintf(stderr, "no decoder for %s/%s\n", avcodec_get_name(par->codec_id), mode);
        return 1;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, par);
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1;
    ctx->pkt_timebase = (AVRational){1, 1000000}; // v4l2m2m drops pts without one
    if (!strcmp(mode, "vaapi")) {
        if (av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", NULL, 0) < 0) {
            fprintf(stderr, "VA-API device failed\n");
            return 1;
        }
        ctx->get_format = pick_vaapi;
    }
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 failed (%s)\n", codec->name);
        return 1;
    }
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc(), *drm = av_frame_alloc();
    int sent = 0, got = 0, mapFail = 0, maxDepth = 0, nready = 0;
    double mapTotal = 0, mapMax = 0;
    char layout[160] = "-";
    const double start = now_ms() + 50;
    for (int eof = 0; !eof || got < sent;) {
        if (!eof) {
            if (av_read_frame(fmt, pkt) < 0) {
                eof = 1;
                avcodec_send_packet(ctx, NULL);
            } else if (pkt->stream_index != si) {
                av_packet_unref(pkt);
                continue;
            } else if (sent < MAX_FRAMES) {
                sleep_until(start + sent * interval);
                pkt->pts = pkt->dts = sent;
                sendAt[sent++] = now_ms();
                if (avcodec_send_packet(ctx, pkt) < 0) fprintf(stderr, "send failed at %d\n", sent - 1);
                av_packet_unref(pkt);
            }
        }
        int r;
        while ((r = avcodec_receive_frame(ctx, frame)) == 0) {
            const double t = now_ms();
            const int64_t i = frame->best_effort_timestamp;
            const int known = i >= 0 && i < sent;
            if (known) lat[got] = t - sendAt[i];
            if (known && sent - 1 - (int)i > maxDepth) maxDepth = sent - 1 - (int)i;
            got++;
            if (frame->format == AV_PIX_FMT_VAAPI) {
                const double m0 = now_ms();
                drm->format = AV_PIX_FMT_DRM_PRIME;
                if (av_hwframe_map(drm, frame, AV_HWFRAME_MAP_READ) < 0)
                    mapFail++;
                else {
                    const double m = now_ms() - m0;
                    mapTotal += m;
                    if (m > mapMax) mapMax = m;
                    // Mapping syncs the surface, so this is when the picture can really be shown.
                    if (known) ready[nready++] = now_ms() - sendAt[i];
                    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)drm->data[0];
                    if (got == 1)
                        snprintf(layout, sizeof layout, "objects=%d layers=%d fourcc=%.4s planes=%d modifier=0x%llx",
                                 d->nb_objects, d->nb_layers, (const char *)&d->layers[0].format,
                                 d->layers[0].nb_planes, (unsigned long long)d->objects[0].format_modifier);
                }
                av_frame_unref(drm);
            } else {
                if (known) ready[nready++] = lat[got - 1];
                if (got == 1) snprintf(layout, sizeof layout, "sysmem %s", av_get_pix_fmt_name(frame->format));
            }
            av_frame_unref(frame);
        }
        if (r == AVERROR_EOF || (eof && now_ms() > start + sent * interval + 2000)) break;
        if (r != AVERROR(EAGAIN)) {
            fprintf(stderr, "receive error %d\n", r);
            break;
        }
    }
    qsort(lat, got, sizeof lat[0], cmp);
    qsort(ready, nready, sizeof ready[0], cmp);
#define Q(a, n, q) ((n) ? (a)[(int)((n) * (q))] : -1.0)
    printf("%s %s %dx%d sent=%d got=%d timed=%d depth=%d | received p50=%.2f | READY ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | map avg=%.2f max=%.2f fail=%d | %s\n",
           codec->name, mode, par->width, par->height, sent, got, nready, maxDepth, Q(lat, got, .5),
           Q(ready, nready, .5), Q(ready, nready, .95), Q(ready, nready, .99), nready ? ready[nready - 1] : -1,
           got - mapFail ? mapTotal / (got - mapFail) : 0, mapMax, mapFail, layout);
    return got == sent ? 0 : 3;
}

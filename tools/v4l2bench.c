// Direct stateful-V4L2 decode-latency bench (the path Moonlight uses), for
// comparison with decbench.c's VA-API numbers. Feeds a clip at its real frame
// rate and measures, per frame, queueing the access unit on OUTPUT to the
// picture being dequeued on CAPTURE. Also checks VIDIOC_EXPBUF, the zero-copy
// hand-off to EGL. Structure follows y700 linux/kernel-y6/iris-restart-test.c.
//
//   v4l2bench /dev/video0 <clip.h264|.hevc|.ivf> [fps]
//
// Build: cc -O2 -o v4l2bench v4l2bench.c $(pkg-config --cflags --libs libavformat libavcodec libavutil) -lm
#define _GNU_SOURCE // ppoll
#include <errno.h>
#include <fcntl.h>
#include <libavformat/avformat.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OUT V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
#define CAP V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define NOUT 8
#define OUTSZ (4 << 20)
#define MAXAU 100000

static int fd, capOn, capPlanes;
static void *outMap[NOUT];
static int outFree[NOUT], nFree;
static double sendAt[MAXAU], ready[MAXAU];
static char capInfo[160] = "-";

static void die(const char *m) {
    perror(m);
    exit(2);
}
static int xioctl(unsigned long r, void *a) {
    int e;
    do e = ioctl(fd, r, a);
    while (e < 0 && errno == EINTR);
    return e;
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}
static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void capSetup(void) {
    struct v4l2_requestbuffers r = {.type = CAP, .memory = V4L2_MEMORY_MMAP};
    struct v4l2_control c = {.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE};
    struct v4l2_format f = {.type = CAP};
    int t = CAP;
    if (capOn && xioctl(VIDIOC_STREAMOFF, &t)) die("STREAMOFF capture");
    capOn = 0;
    if (xioctl(VIDIOC_REQBUFS, &r)) die("REQBUFS capture 0");
    if (xioctl(VIDIOC_G_FMT, &f)) die("G_FMT capture");
    const unsigned native = f.fmt.pix_mp.pixelformat;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12; // linear: imports into EGL without a modifier
    if (xioctl(VIDIOC_S_FMT, &f)) die("S_FMT capture NV12");
    capPlanes = f.fmt.pix_mp.num_planes;
    r.count = (xioctl(VIDIOC_G_CTRL, &c) ? 4 : c.value) + 4;
    if (xioctl(VIDIOC_REQBUFS, &r)) die("REQBUFS capture");
    struct v4l2_exportbuffer e = {.type = CAP, .index = 0, .plane = 0, .flags = O_RDONLY | O_CLOEXEC};
    const int exported = !xioctl(VIDIOC_EXPBUF, &e);
    if (exported) close(e.fd);
    snprintf(capInfo, sizeof capInfo, "capture %ux%u %.4s (native %.4s) planes=%d stride=%u bufs=%u expbuf=%s",
             f.fmt.pix_mp.width, f.fmt.pix_mp.height, (char *)&f.fmt.pix_mp.pixelformat, (const char *)&native,
             capPlanes, f.fmt.pix_mp.plane_fmt[0].bytesperline, r.count, exported ? "ok" : strerror(errno));
    for (unsigned i = 0; i < r.count; i++) {
        struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
        struct v4l2_buffer b = {.type = CAP, .memory = V4L2_MEMORY_MMAP, .index = i, .m.planes = pl, .length = capPlanes};
        if (xioctl(VIDIOC_QBUF, &b)) die("QBUF capture");
    }
    if (xioctl(VIDIOC_STREAMON, &t)) die("STREAMON capture");
    capOn = 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s /dev/videoN <clip> [fps]\n", argv[0]);
        return 2;
    }
    const double interval = 1000.0 / (argc > 3 ? atof(argv[3]) : 120.0);
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, argv[2], NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) die(argv[2]);
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (si < 0) return 2;
    const AVCodecParameters *par = fmt->streams[si]->codecpar;
    unsigned pixfmt;
    switch (par->codec_id) {
    case AV_CODEC_ID_H264: pixfmt = V4L2_PIX_FMT_H264; break;
    case AV_CODEC_ID_HEVC: pixfmt = V4L2_PIX_FMT_HEVC; break;
    case AV_CODEC_ID_AV1: pixfmt = v4l2_fourcc('A', 'V', '0', '1'); break;
    default: fprintf(stderr, "unsupported codec\n"); return 2;
    }
    // Load every access unit first so file I/O never lands inside a measurement.
    static AVPacket *au[MAXAU];
    int n = 0;
    for (AVPacket *p = av_packet_alloc(); n < MAXAU && av_read_frame(fmt, p) >= 0; p = av_packet_alloc()) {
        if (p->stream_index == si && p->size <= OUTSZ) au[n++] = p;
        else av_packet_free(&p);
    }

    fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (fd < 0) die(argv[1]);
    struct v4l2_format f = {.type = OUT};
    f.fmt.pix_mp.pixelformat = pixfmt;
    f.fmt.pix_mp.width = par->width;
    f.fmt.pix_mp.height = par->height;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = OUTSZ;
    if (xioctl(VIDIOC_S_FMT, &f)) die("S_FMT output");
    struct v4l2_requestbuffers r = {.type = OUT, .memory = V4L2_MEMORY_MMAP, .count = NOUT};
    if (xioctl(VIDIOC_REQBUFS, &r) || r.count < NOUT) die("REQBUFS output");
    for (int i = 0; i < NOUT; i++) {
        struct v4l2_plane pl = {0};
        struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_MMAP, .index = i, .m.planes = &pl, .length = 1};
        if (xioctl(VIDIOC_QUERYBUF, &b)) die("QUERYBUF output");
        outMap[i] = mmap(NULL, pl.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl.m.mem_offset);
        if (outMap[i] == MAP_FAILED) die("mmap output");
        outFree[nFree++] = i;
    }
    struct v4l2_event_subscription sub = {.type = V4L2_EVENT_SOURCE_CHANGE};
    if (xioctl(VIDIOC_SUBSCRIBE_EVENT, &sub)) die("subscribe source change");
    int t = OUT;
    if (xioctl(VIDIOC_STREAMON, &t)) die("STREAMON output");

    int next = 0, got = 0, late = 0, sourceChanges = 0;
    const double start = now_ms() + 20;
    double lastProgress = start;
    while (got < n && now_ms() - lastProgress < 2000) {
        double tnow = now_ms();
        if (next < n && tnow >= start + next * interval) {
            if (!nFree) late++; // decoder still holds every bitstream buffer: back-pressure
            else {
                const int idx = outFree[--nFree];
                struct v4l2_plane pl = {.bytesused = au[next]->size};
                struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_MMAP, .index = idx, .m.planes = &pl, .length = 1};
                memcpy(outMap[idx], au[next]->data, au[next]->size);
                b.timestamp.tv_sec = next; // CAPTURE carries it back
                sendAt[next] = now_ms();
                if (xioctl(VIDIOC_QBUF, &b)) die("QBUF output");
                next++;
            }
        }
        tnow = now_ms();
        double wait = next < n ? start + next * interval - tnow : 50;
        if (wait < 0) wait = 0;
        struct timespec ts = {(time_t)(wait / 1e3), (long)(fmod(wait, 1e3) * 1e6)};
        struct pollfd p = {.fd = fd, .events = POLLIN | POLLOUT | POLLPRI};
        if (ppoll(&p, 1, &ts, NULL) < 0) die("poll");
        if (p.revents & POLLPRI) {
            struct v4l2_event ev;
            while (!xioctl(VIDIOC_DQEVENT, &ev))
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    sourceChanges++;
                    capSetup();
                }
        }
        if (p.revents & POLLOUT)
            for (;;) {
                struct v4l2_plane pl = {0};
                struct v4l2_buffer b = {.type = OUT, .memory = V4L2_MEMORY_MMAP, .m.planes = &pl, .length = 1};
                if (xioctl(VIDIOC_DQBUF, &b)) break;
                outFree[nFree++] = b.index;
            }
        if ((p.revents & POLLIN) && capOn)
            for (;;) {
                struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
                struct v4l2_buffer b = {.type = CAP, .memory = V4L2_MEMORY_MMAP, .m.planes = pl, .length = capPlanes};
                if (xioctl(VIDIOC_DQBUF, &b)) break;
                const long i = b.timestamp.tv_sec;
                if (pl[0].bytesused && i >= 0 && i < next) {
                    ready[got++] = now_ms() - sendAt[i];
                    lastProgress = now_ms();
                }
                if (xioctl(VIDIOC_QBUF, &b)) die("re-QBUF capture");
            }
    }
    // The first pictures wait for capture setup after SOURCE_CHANGE; keep them out of the statistics.
    const int skip = got > 10 ? 5 : 0, m = got - skip;
    qsort(ready + skip, m, sizeof ready[0], cmp);
#define Q(q) (m > 0 ? ready[skip + (int)(m * (q))] : -1.0)
    printf("v4l2 %s %dx%d sent=%d got=%d late=%d srcchg=%d | READY ms p50=%.2f p95=%.2f p99=%.2f max=%.2f | %s\n",
           avcodec_get_name(par->codec_id), par->width, par->height, next, got, late, sourceChanges, Q(.5), Q(.95),
           Q(.99), m > 0 ? ready[skip + m - 1] : -1, capInfo);
    return got == n ? 0 : 3;
}

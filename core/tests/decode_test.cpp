// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware self-test of media_linux.cpp, no network: feeds a clip through
// uu::Media at its frame rate while a "player" thread takes pictures the way
// the renderer will (wait, acquire, hold one, release the previous one).
//
//   decode_test <clip.h264|.hevc> [fps]
//
// Phases: 1) whole clip; 2) reset, whole clip again (decoder reopen);
// 3) reset, start mid-GOP: nothing may show before the next IDR and a
// keyframe must be requested. Prints per-phase latency from Media::video()
// to the picture being acquirable, and exits 1 on any failed check.
#include "bridge.h"
#include "media.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>
extern "C" {
#include <libavformat/avformat.h>
}

namespace {
int64_t nowUs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000;
}
int failures = 0;
void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <clip.h264|.hevc> [fps]\n", argv[0]);
        return 2;
    }
    const double fps = argc > 2 ? std::atof(argv[2]) : 120.0;
    AVFormatContext *fmt = nullptr;
    if (avformat_open_input(&fmt, argv[1], nullptr, nullptr) < 0 || avformat_find_stream_info(fmt, nullptr) < 0)
        return std::perror(argv[1]), 2;
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0) return 2;
    const auto *par = fmt->streams[si]->codecpar;
    const bool hevc = par->codec_id == AV_CODEC_ID_HEVC;
    if (!hevc && par->codec_id != AV_CODEC_ID_H264) return std::fprintf(stderr, "H.264/HEVC only\n"), 2;
    std::vector<std::vector<uint8_t>> units;
    std::vector<bool> keys;
    for (AVPacket *p = av_packet_alloc(); av_read_frame(fmt, p) >= 0; av_packet_unref(p))
        if (p->stream_index == si) {
            units.emplace_back(p->data, p->data + p->size);
            keys.push_back(p->flags & AV_PKT_FLAG_KEY);
        }
    const size_t n = units.size();
    size_t midStart = n / 2, nextKey = n;
    while (midStart < n && keys[midStart]) ++midStart;
    for (size_t i = midStart; i < n; ++i)
        if (keys[i]) {
            nextKey = i;
            break;
        }
    std::printf("%s %dx%d, %zu access units, keyframe after mid-GOP start %zu: %zu\n", hevc ? "HEVC" : "H.264",
                par->width, par->height, n, midStart, nextKey);

    uu::Media media;
    std::atomic<bool> stop{false};
    std::mutex seenMutex; // guards the four below
    std::vector<int64_t> handoffUs; // picture ready -> player has it
    uint64_t acquired = 0;
    yy_frame shape{};
    bool contentChecked = false, contentOk = false;
    std::thread player([&] {
        yy_frame held{};
        held.fd = -1;
        uint64_t last = 0;
        while (!stop) {
            if (!yy_frame_wait(last, 50)) continue;
            yy_frame f{};
            if (!yy_frame_acquire(&f)) continue;
            const int64_t t = nowUs();
            last = f.serial;
            std::lock_guard lock(seenMutex);
            ++acquired;
            shape = f;
            if (!contentChecked) {
                const size_t bytes = size_t(f.uv_offset) + size_t(f.stride) * f.height / 2;
                if (void *m = mmap(nullptr, bytes, PROT_READ, MAP_SHARED, f.fd, 0); m != MAP_FAILED) {
                    const auto *y = static_cast<const uint8_t *>(m);
                    uint64_t sum = 0;
                    for (uint32_t row = 0; row < f.height; row += 16) sum += y[size_t(row) * f.stride + f.width / 2];
                    contentOk = sum > 0;
                    munmap(m, bytes);
                }
                contentChecked = true;
            }
            handoffUs.push_back(t - f.ready_us);
            if (held.fd >= 0) {
                close(held.fd);
                yy_frame_release(&held);
            }
            held = f;
        }
        if (held.fd >= 0) {
            close(held.fd);
            yy_frame_release(&held);
        }
    });

    auto run = [&](const char *name, size_t from) {
        {
            std::lock_guard lock(seenMutex);
            handoffUs.clear();
            acquired = 0;
        }
        const int64_t start = nowUs() + 20000;
        for (size_t i = from; i < n; ++i) {
            const int64_t due = start + int64_t((i - from) * 1e6 / fps);
            while (nowUs() < due) std::this_thread::sleep_for(std::chrono::microseconds(200));
            media.video(units[i].data(), units[i].size(), media.generation(), std::nullopt, hevc);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto s = media.stats();
        std::lock_guard lock(seenMutex);
        auto lat = handoffUs;
        std::sort(lat.begin(), lat.end());
        auto q = [&](double p) { return lat.empty() ? -1.0 : lat[size_t(double(lat.size() - 1) * p)] / 1000.0; };
        std::printf("%s: fed %zu, decoded %llu, acquired %llu, dropped %llu, errors %llu | decoder->player "
                    "p50 %.2f p95 %.2f max %.2f ms | decode %.2f ms (EMA) | %ux%u stride %u uv %u | %s\n",
                    name, n - from, (unsigned long long)s.decodedTotal, (unsigned long long)acquired,
                    (unsigned long long)s.droppedFrames, (unsigned long long)s.decodeErrors, q(.5), q(.95),
                    lat.empty() ? -1.0 : lat.back() / 1000.0, s.decodeMs, shape.width, shape.height, shape.stride,
                    shape.uv_offset, media.status().c_str());
        return s;
    };

    auto s1 = run("phase 1 whole clip", 0);
    check(s1.decodedTotal + 2 >= n, "phase 1 decoded every access unit");
    check(s1.decodeErrors == 0, "phase 1 without decode errors");
    check(shape.width == uint32_t(par->width) && shape.height == uint32_t(par->height), "visible size matches the clip");
    check(shape.fd >= 0 && shape.stride >= shape.width && shape.uv_offset >= shape.stride * shape.height,
          "NV12 layout is sane");
    check(contentChecked && contentOk, "DMA-BUF maps and holds a picture");
    check(acquired > n / 2, "player got most pictures");
    check(s1.decodeMs > 0 && s1.decodeMs < 16, "decode latency under 16 ms");

    media.reset();
    auto s2 = run("phase 2 after reset", 0);
    check(s2.decodedTotal + 2 >= n, "phase 2 decoded every access unit after reopen");

    media.reset();
    media.takeKeyframeRequest();
    auto s3 = run("phase 3 mid-GOP start", midStart);
    check(media.takeKeyframeRequest() || nextKey == n, "keyframe requested while waiting for an IDR");
    check(s3.droppedFrames >= nextKey - midStart, "units before the IDR were dropped");
    check(s3.decodedTotal + 2 >= n - nextKey && s3.decodedTotal <= n - nextKey, "decoding resumed at the IDR");

    stop = true;
    player.join();
    std::printf(failures ? "%d check(s) FAILED\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}

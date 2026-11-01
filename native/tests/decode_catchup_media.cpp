// SPDX-License-Identifier: GPL-3.0-or-later
#include "media.hpp"
#include <atomic>
#include <chrono>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
}

// The real decoders process every reference packet. A short test-only fence
// makes input accumulation deterministic without overwhelming the queue limit.
static std::atomic<bool> firstSend{true};
static std::atomic<bool> slowReceive{true};
static std::atomic<int> faultKind{0};
extern "C" int avcodec_send_packet(AVCodecContext *ctx, const AVPacket *pkt) {
    using Send = int (*)(AVCodecContext *, const AVPacket *);
    static auto real = reinterpret_cast<Send>(dlsym(RTLD_NEXT, "avcodec_send_packet"));
    if (firstSend.exchange(false)) std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if (pkt && pkt->pts == 18000 && faultKind == 1) return AVERROR_INVALIDDATA;
    return real(ctx, pkt);
}
extern "C" int avcodec_receive_frame(AVCodecContext *ctx, AVFrame *frame) {
    using Receive = int (*)(AVCodecContext *, AVFrame *);
    static auto real = reinterpret_cast<Receive>(dlsym(RTLD_NEXT, "avcodec_receive_frame"));
    int result = real(ctx, frame);
    if (result == 0 && slowReceive) std::this_thread::sleep_for(std::chrono::milliseconds(3));
    if (result == 0 && frame->pts == 17250) {
        if (faultKind == 2) return AVERROR_INVALIDDATA;
        if (faultKind == 3) frame->decode_error_flags = FF_DECODE_ERROR_INVALID_BITSTREAM;
    }
    return result;
}
static void check(bool ok, const char *what) {
    if (!ok) throw std::runtime_error(what);
}
int main(int argc, char **argv) {
    try {
        check(argc == 4 || argc == 5, "usage: test <24-AU pack> h264|hevc|av1 candidate|baseline [empty-tail|send-error|receive-error|damaged]");
        std::string codec = argv[2];
        bool candidate = std::string(argv[3]) == "candidate";
        const std::string mode = argc == 5 ? argv[4] : "";
        faultKind = mode == "send-error" ? 1 : mode == "receive-error" ? 2 : mode == "damaged" ? 3 : 0;
        std::vector<std::vector<uint8_t>> packets;
        std::ifstream input(argv[1], std::ios::binary);
        uint32_t size;
        while (input.read(reinterpret_cast<char *>(&size), sizeof(size))) {
            check(size && size < 1024 * 1024, "invalid AU size");
            packets.emplace_back(size);
            check(bool(input.read(reinterpret_cast<char *>(packets.back().data()), size)), "truncated AU");
        }
        check(packets.size() == 24, "expected 24 reference-chain AUs");
        check(SDL_Init(SDL_INIT_AUDIO) == 0, "SDL initialization failed");
        {
            uu::Media media;
            media.setYuvOutput(true);
            media.setTargetFps(120);
            media.setPlayoutDelay(0);
            for (int run = 0; run < 2; ++run) {
                uint64_t old = media.generation();
                if (run) {
                    media.reset();
                    slowReceive = false;
                    faultKind = 0;
                    media.video(packets[0].data(), packets[0].size(), old, 0);
                    check(media.stats().inputQueued == 0, "stale generation admitted");
                }
                const auto epoch = media.generation();
                for (size_t i = 0; i < packets.size(); ++i) {
                    media.video(packets[i].data(), packets[i].size(), epoch,
                                uint32_t(i * 750), codec == "hevc", std::nullopt, codec == "av1");
                    if (run) std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                if ((mode == "empty-tail" || mode == "send-error") && !run) {
                    // These are valid framing units with no coded image. The latest
                    // good frame must survive even when the final AU emits nothing.
                    const std::vector<uint8_t> empty = codec == "h264"
                        ? std::vector<uint8_t>{0,0,0,1,9,0x10}
                        : codec == "hevc" ? std::vector<uint8_t>{0,0,0,1,0x46,1,0x50}
                                         : std::vector<uint8_t>{0x12,0};
                    media.video(empty.data(), empty.size(), epoch, 18000,
                                codec == "hevc", std::nullopt, codec == "av1");
                }
                uint64_t serial = 0, hash = 0;
                int64_t lastTimestamp = -1;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (std::chrono::steady_clock::now() < deadline) {
                    auto frame = media.picture(serial);
                    if (frame.serial) {
                        serial = frame.serial;
                        check(frame.mediaTimestamp >= lastTimestamp, "reordered output");
                        lastTimestamp = frame.mediaTimestamp;
                        check(bool(frame.pixels), "missing CPU image");
                        hash = 1469598103934665603ull;
                        for (auto byte : *frame.pixels) hash = (hash ^ byte) * 1099511628211ull;
                    }
                    auto stats = media.stats();
                    const size_t expected = packets.size() - (mode == "receive-error" && !run ? 1 : 0);
                    if (stats.inputQueued == 0 && stats.decodedTotal >= expected) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                // Consume the tail publication made alongside the final decoded count.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                auto tail = media.picture(serial);
                if (tail.serial) {
                    lastTimestamp = tail.mediaTimestamp;
                    hash = 1469598103934665603ull;
                    for (auto byte : *tail.pixels) hash = (hash ^ byte) * 1099511628211ull;
                }
                auto stats = media.stats();
                std::cout << codec << " run=" << run << " decoded=" << stats.decodedTotal
                          << " published=" << stats.publishedTotal << " skipped=" << stats.presentationSkipped
                          << " errors=" << stats.decodeErrors << " tail_ts=" << lastTimestamp
                          << " hash=" << hash << std::endl;
                const uint64_t allowedErrors = argc == 5 && !run ? 1 : 0;
                check(stats.decodeErrors <= allowedErrors && stats.droppedFrames == 0, "reference decode failed");
                const size_t expectedDecoded = packets.size() - (mode == "receive-error" && !run ? 1 : 0);
                const size_t expectedTail = packets.size() - (!run && (mode == "receive-error" || mode == "damaged") ? 2 : 1);
                check(stats.inputQueued == 0 && stats.decodedTotal == expectedDecoded, "did not decode expected AUs");
                check(lastTimestamp == int64_t(expectedTail * 750), "did not publish last good queue tail");
                if (candidate && !run) check(stats.publishedTotal < stats.decodedTotal, "backlog never skipped work");
                if (run) check(stats.publishedTotal == stats.decodedTotal, "idle cadence discarded frames");
            }
        }
        SDL_Quit();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        SDL_Quit();
        return 1;
    }
}

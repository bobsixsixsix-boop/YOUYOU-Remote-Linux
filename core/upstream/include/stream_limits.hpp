#pragma once
#include <algorithm>
#include <cstdint>
#include <stdexcept>

// YOUYOU Linux: frame rates above 60 and any even stream size, reviewed from
// the v0.1.15 high_fps / custom_resolution patches against GameViewer 4.44.
namespace uu {
inline constexpr int maxCaptureFps = 164;
constexpr bool supportedCaptureRate(int fps) {
    return fps == 30 || fps == 60 || fps == 120 || fps == 144 || fps == 164;
}
// gv_pb.FPS ends at the 144 tier (4). Faster requests use that tier and carry
// the exact rate in CaptureParams.fpsCount (connect field 11, RPC field 18).
inline int captureRateTier(int fps) {
    if (!supportedCaptureRate(fps)) throw std::invalid_argument("Unsupported capture rate");
    return fps == 30 ? 1 : fps == 60 ? 2 : 4;
}
constexpr int fpsFromTier(int tier) {
    return tier == 1 ? 30 : tier == 2 ? 60 : tier == 3 ? 90 : tier == 4 ? 144 : 0;
}
// FpsSettingRequest only has the enum, so 120 and 164 need a capture request.
constexpr bool enumOnlyCaptureRate(int fps) { return fps == 30 || fps == 60; }
// Even sizes up to 4096x2160, what the Y700's decoder takes.
constexpr bool streamSize(int w, int h) {
    return std::min(w, h) >= 180 && std::max(w, h) >= 320 && std::min(w, h) <= 2160 &&
           std::max(w, h) <= 4096 && w % 2 == 0 && h % 2 == 0;
}
// SDP levels from the luma sample rate (H.264/H.265 Annex A). 1080p keeps the
// original offers that existing hosts interoperate with.
struct StreamLevels { const char *avcHex; int hevc; };
inline StreamLevels streamLevels(int w, int h, int fps) {
    if (fps <= 120 && std::max(w, h) <= 1920 && std::min(w, h) <= 1080) return {"1f", 123};
    const int macroblocks = ((w + 15) / 16) * ((h + 15) / 16), perSecond = macroblocks * fps;
    const char *avc = macroblocks <= 22080 && perSecond <= 589824 ? "32" : perSecond <= 983040 ? "33" :
                      perSecond <= 2073600 ? "34" : perSecond <= 4177920 ? "3c" : "3d";
    const auto samples = int64_t(w) * h * fps;
    return {avc, samples <= 267386880 ? 150 : samples <= 534773760 ? 153 : samples <= 1069547520 ? 156 : 183};
}
}

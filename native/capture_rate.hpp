#pragma once
#include <algorithm>
#include <stdexcept>

namespace uu {
inline constexpr int maxCaptureFps = 164;
constexpr bool supportedCaptureRate(int fps) {
    return fps == 30 || fps == 60 || fps == 120 || fps == 144 || fps == 164;
}

// gv_pb.FPS in the released GameViewer 4.44 protocol ends at the 144 tier.
// Keep higher requests in that tier and carry the exact rate separately
// in CaptureParams.fpsCount (11) and CaptureSettingRequest.fpsCount (18).
inline int captureRateTier(int fps) {
    switch (fps) {
    case 30: return 1;
    case 60: return 2;
    case 120: case 144: case 164: return 4;
    default: throw std::invalid_argument("Unsupported capture rate");
    }
}

constexpr int fpsFromTier(int value) {
    switch (value) {
    case 1: return 30;
    case 2: return 60;
    case 3: return 90;
    case 4: return 144;
    default: return 0;
    }
}

constexpr bool enumOnlyCaptureRate(int fps) { return fps == 30 || fps == 60; }

// A tier-4 sync cannot distinguish requests such as 120, 144 and 164 FPS.
// Never turn it into exact-rate evidence for a different requested rate.
constexpr int captureRateSyncEvidence(int requestedFps, int tier) {
    return tier == 4 && requestedFps != 144 ? 0 : fpsFromTier(tier);
}

// Accommodate 100 ms of intentional hold plus two burst frames. The decoder's
// independent byte bound remains in force at large video dimensions.
constexpr int capturePlayoutFrameBudget(int fps) {
    return std::max(8, (std::clamp(fps, 30, maxCaptureFps) + 9) / 10 + 2);
}
}

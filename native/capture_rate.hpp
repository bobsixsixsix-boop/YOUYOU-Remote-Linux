#pragma once
#include <stdexcept>

namespace uu {
constexpr bool supportedCaptureRate(int fps) {
    return fps == 30 || fps == 60 || fps == 120;
}

// gv_pb.FPS in the released GameViewer 4.44 protocol has no 120 enum.
// Use the next available capability tier and carry the exact rate separately
// in CaptureParams.fpsCount (11) and CaptureSettingRequest.fpsCount (18).
inline int captureRateTier(int fps) {
    switch (fps) {
    case 30: return 1;
    case 60: return 2;
    case 120: return 4;
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
}

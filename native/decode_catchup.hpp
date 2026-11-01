// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace uu {
// All access units still reach the decoder in order. Only the expensive output
// transfer/conversion is omitted while newer encoded work is waiting.
inline bool deferDecodedOutput(std::size_t waiting, int64_t queuedMs, int fps,
                               int64_t now, int64_t lastPublishedMs) {
    const int rate = std::clamp(fps, 1, 120);
    const int frameMs = (1000 + rate - 1) / rate;
    const bool behind = waiting >= 3 || queuedMs >= std::max(16, 2 * frameMs);
    return waiting != 0 && behind && lastPublishedMs >= 0 &&
           now >= lastPublishedMs && now - lastPublishedMs < 33;
}
}

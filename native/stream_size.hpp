#pragma once
#include <algorithm>
#include <cstdint>

namespace uu {
inline bool decodableSize(int width,int height) {
    return std::min(width,height)>0 && std::min(width,height)<=2160 && std::max(width,height)<=4096;
}
inline bool streamSize(int width,int height) {
    return decodableSize(width,height) && std::min(width,height)>=180 && std::max(width,height)>=320 &&
           width%2==0 && height%2==0;
}

// The original 1080p offers interoperate with existing UU hosts. For larger
// pictures allow the sample rates from AVC Annex A, HEVC Annex A and AV1 Annex A.
struct StreamLevels { const char *avcHex; int hevc, av1; };
inline StreamLevels streamLevels(int width,int height,int fps) {
    if(std::max(width,height)<=1920 && std::min(width,height)<=1080) return {"1f",123,13};
    const int macroblocks=((width+15)/16)*((height+15)/16);
    const int macroblocksPerSecond=macroblocks*fps;
    const char *avc=macroblocks<=22080 && macroblocksPerSecond<=589824?"32":
                    macroblocksPerSecond<=983040?"33":macroblocksPerSecond<=2073600?"34":"3c";
    const auto samples=int64_t(width)*height*fps;
    return {avc,samples<=267386880?150:samples<=534773760?153:156,samples<=534773760?13:14};
}
}

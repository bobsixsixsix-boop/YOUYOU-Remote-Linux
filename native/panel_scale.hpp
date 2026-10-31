#pragma once
#include <stream_size.hpp>
#include <array>
#include <utility>

namespace uu {
struct PanelScale {
    const char *label;
    int shortEdge;
    int percent;
};
inline constexpr std::array<PanelScale,6> panelScales{{
    {"720p",720,0},{"1080p",1080,0},{"1440p",1440,0},
    {"半尺寸 · 50%",0,50},{"75%",0,75},{"原生 · 100%",0,100}
}};

// Both axes use one linear factor, then round to the nearest even number
// of pixels required by 4:2:0 encoders. Portrait targets keep their order.
inline std::pair<int,int> panelVideoSize(int width,int height,PanelScale scale) {
    const int shortSide=std::min(width,height);
    if(shortSide<=0 || scale.shortEdge>shortSide)return {};
    const int numerator=scale.shortEdge?scale.shortEdge:scale.percent;
    const int denominator=scale.shortEdge?shortSide:100;
    const auto even=[&](int n){return int((int64_t(n)*numerator+denominator)/(2LL*denominator)*2);};
    const auto size=std::make_pair(even(width),even(height));
    return streamSize(size.first,size.second)?size:std::pair<int,int>{};
}
}

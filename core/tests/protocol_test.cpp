// Self-test of the Linux additions to the upstream protocol code: frame rates and
// stream sizes (stream_limits.hpp), the screen list fields the dedicated desktop
// reads, and the desktop size request. Run: build/protocol_test
#undef NDEBUG // asserts are the test, also in Release builds
#include "quality_rpc.hpp"
#include "stream_limits.hpp"
#include <cassert>
#include <cstdio>
#include <stdexcept>

using uu::Bytes;
static void var(Bytes &b, uint64_t v) { while (v > 127) { b.push_back(uint8_t(v) | 128); v >>= 7; } b.push_back(uint8_t(v)); }
static void integer(Bytes &b, unsigned key, uint64_t v) { var(b, key * 8); var(b, v); }
static void message(Bytes &b, unsigned key, const Bytes &v) { var(b, key * 8 + 2); var(b, v.size()); b.insert(b.end(), v.begin(), v.end()); }
static Bytes size(int w, int h) { Bytes s; integer(s, 3, w); integer(s, 4, h); return s; }

int main() {
    assert(uu::captureRateTier(30) == 1 && uu::captureRateTier(60) == 2 && uu::captureRateTier(120) == 4 && uu::captureRateTier(164) == 4);
    bool threw = false;
    try { uu::captureRateTier(90); } catch (const std::invalid_argument &) { threw = true; }
    assert(threw);
    assert(uu::streamSize(2280, 1428) && uu::streamSize(1428, 2280) && !uu::streamSize(2281, 1428) && !uu::streamSize(4098, 2160));
    assert(uu::streamLevels(1920, 1080, 60).hevc == 123 && uu::streamLevels(2560, 1600, 120).hevc == 153);

    // Main.screens: a physical screen 0 and the session's virtual screen 14 (current)
    Bytes physical, virt, sources, main;
    integer(physical, 1, 0); message(physical, 4, size(2560, 1440));
    integer(virt, 1, 14); integer(virt, 5, 1); message(virt, 4, size(2560, 1440));
    message(virt, 3, size(2280, 1428)); message(virt, 3, size(2560, 1600));
    Bytes scaled; integer(scaled, 3, 1280); integer(scaled, 4, 720); integer(scaled, 5, 2560); integer(scaled, 6, 1440);
    message(virt, 3, scaled); // pixel size wins over the desktop size
    message(sources, 1, physical); message(sources, 1, virt); integer(sources, 2, 14);
    message(main, 7, sources);
    const auto s = uu::parseScreenSources(main.data(), main.size());
    assert(s && s->screenId == 14 && s->count == 2 && s->type == 1 && s->width == 2560 && s->height == 1440);
    assert(s->modes.size() == 3 && s->modes[0] == std::make_pair(2280, 1428) && s->modes[2] == std::make_pair(2560, 1440));

    // a malformed size keeps the screen id
    Bytes odd, oddSources, oddMain;
    integer(odd, 1, 3); integer(odd, 4, 7); integer(oddSources, 2, 3);
    message(oddSources, 1, odd); message(oddMain, 7, oddSources);
    const auto o = uu::parseScreenSources(oddMain.data(), oddMain.size());
    assert(o && o->screenId == 3 && o->width == 0);

    assert(!uu::desktopModeRpc(1, 14, 2280, 1428, 120, 6, 60).empty());
    for (auto bad : {+[] { uu::desktopModeRpc(1, 14, 2281, 1428, 120, 3); }, +[] { uu::desktopModeRpc(1, -1, 2280, 1428, 120, 3); },
                     +[] { uu::desktopModeRpc(1, 14, 2280, 1428, 90, 3); }, +[] { uu::desktopModeRpc(1, 14, 2280, 1428, 120, 6, 0); }}) {
        threw = false;
        try { bad(); } catch (const std::invalid_argument &) { threw = true; }
        assert(threw);
    }
    std::puts("protocol_test ok");
}

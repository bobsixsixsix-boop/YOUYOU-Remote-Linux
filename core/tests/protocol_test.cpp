// Self-test of the Linux additions to the upstream protocol code: frame rates and
// stream sizes (stream_limits.hpp), the screen list fields the dedicated desktop
// reads (sizes, display scales, every screen), and the desktop size, scale and
// screen selection and super screen exit and return requests. Run: build/protocol_test
#undef NDEBUG // asserts are the test, also in Release builds
#include "quality_rpc.hpp"
#include "stream_limits.hpp"
#include <algorithm>
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
    integer(physical, 1, 0); integer(physical, 2, 165); message(physical, 4, size(2560, 1440));
    integer(virt, 1, 14); integer(virt, 5, 1); message(virt, 4, size(2560, 1440));
    message(virt, 3, size(2280, 1428)); message(virt, 3, size(2560, 1600));
    Bytes scaled; integer(scaled, 3, 1280); integer(scaled, 4, 720); integer(scaled, 5, 2560); integer(scaled, 6, 1440);
    message(virt, 3, scaled); // pixel size wins over the desktop size
    Bytes dpi, packed; // DpiScale: current 100, recommended 150, offered 100..200 packed
    for (int v : {100, 125, 150, 175, 200}) var(packed, v);
    integer(dpi, 1, 100); integer(dpi, 2, 150); message(dpi, 3, packed); message(virt, 9, dpi);
    message(sources, 1, physical); message(sources, 1, virt); integer(sources, 2, 14);
    message(main, 7, sources);
    const auto s = uu::parseScreenSources(main.data(), main.size());
    assert(s && s->screenId == 14 && s->count == 2 && s->type == 1 && s->width == 2560 && s->height == 1440);
    assert(s->modes.size() == 3 && s->modes[0] == std::make_pair(2280, 1428) && s->modes[2] == std::make_pair(2560, 1440));
    assert(s->scale == 100 && s->recommendedScale == 150 && s->scales.size() == 5 && s->scales[4] == 200);
    // every screen, in the host's order, for the monitor switch
    assert(s->all.size() == 2 && s->all[0].id == 0 && s->all[0].type == 0 && s->all[0].width == 2560 && s->all[0].height == 1440);
    assert(s->all[0].fps == 165 && s->all[1].id == 14 && s->all[1].type == 1 && s->all[1].width == 2560 && s->all[1].fps == 0);
    Bytes gone, goneMain; // current id 9 is not in the list: no current screen, the list stays
    message(gone, 1, physical); integer(gone, 2, 9); message(goneMain, 7, gone);
    const auto g = uu::parseScreenSources(goneMain.data(), goneMain.size());
    assert(g && g->screenId == -1 && g->count == 1 && g->all.size() == 1 && g->all[0].id == 0);

    // a malformed size or scale list keeps the screen id; unpacked scales parse too
    Bytes odd, oddDpi, oddSources, oddMain;
    integer(odd, 1, 3); integer(odd, 4, 7); integer(oddSources, 2, 3);
    integer(oddDpi, 1, 200); integer(oddDpi, 3, 100); integer(oddDpi, 3, 200); message(odd, 9, oddDpi);
    message(oddSources, 1, odd); message(oddMain, 7, oddSources);
    const auto o = uu::parseScreenSources(oddMain.data(), oddMain.size());
    assert(o && o->screenId == 3 && o->width == 0 && o->scale == 200 && o->scales == std::vector<int>({100, 200}));
    Bytes cut, cutSources, cutMain; // DpiScale with a truncated packed list
    integer(cut, 1, 4); message(cut, 9, Bytes{0x1a, 0x01, 0xc8}); integer(cutSources, 2, 4);
    message(cutSources, 1, cut); message(cutMain, 7, cutSources);
    const auto c = uu::parseScreenSources(cutMain.data(), cutMain.size());
    assert(c && c->screenId == 4 && c->scales.empty());

    // the scale request is the quality update plus dpi_scale (field 9)
    const auto rpc = uu::qualityUpdateRpc(1, 120, 6, 14, 60, 200);
    const Bytes tag{0x48, 0xc8, 0x01};
    assert(std::search(rpc.begin(), rpc.end(), tag.begin(), tag.end()) != rpc.end());
    const auto plain = uu::qualityUpdateRpc(1, 120, 6, 14, 60);
    assert(std::search(plain.begin(), plain.end(), tag.begin(), tag.end()) == plain.end());

    // selecting a screen as the official app does: screen 5, resolution -1/-1, ChooseType_DEFAULT, codec
    const auto has = [](const Bytes &b, const Bytes &tag) { return std::search(b.begin(), b.end(), tag.begin(), tag.end()) != b.end(); };
    const auto pick = uu::selectScreenRpc(7, 5, 60, 3, true, 1920, 1080);
    const Bytes minusOne{0x28, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01};
    assert(has(pick, {0x20, 0x05}) && has(pick, minusOne) && has(pick, {0x50, 0x01}) && has(pick, {0x68, 0x02}));
    assert(has(uu::selectScreenRpc(7, 5, 60, 3, false, 1920, 1080), {0x68, 0x01}));
    // back to super screen: RpcRequest{header{id 7}, enter_super_screen{reason MANUAL, resolution{2280, 1428}}}
    const auto enter = uu::enterSuperScreenRpc(7, 2280, 1428);
    const Bytes enterBody{0xaa, 0x01, 0x11, 0x0a, 0x02, 0x08, 0x07, 0xd2, 0x01, 0x0a, 0x08, 0x01, 0x12, 0x06, 0x08, 0xe8, 0x11, 0x10, 0x94, 0x0b};
    assert(enter.size() > enterBody.size() && std::equal(enterBody.begin(), enterBody.end(), enter.end() - enterBody.size()));
    // leaving super screen: RpcRequest{header{id 7}, quit_super_screen{}}, and its answer
    const auto quit = uu::quitSuperScreenRpc(7);
    const Bytes quitBody{0xaa, 0x01, 0x06, 0x0a, 0x02, 0x08, 0x07, 0x72, 0x00};
    assert(quit.size() > quitBody.size() && std::equal(quitBody.begin(), quitBody.end(), quit.end() - quitBody.size()));
    Bytes head, quitCode, resp, reply; integer(head, 1, 7); integer(quitCode, 1, 3);
    message(resp, 1, head); message(resp, 10, quitCode); message(reply, 22, resp);
    const auto a = uu::parseCaptureRpcAck(reply.data(), reply.size());
    assert(a && a->requestId == 7 && a->superScreen && a->errors == 1 && a->errorCode == 3 && !a->otherFields);

    assert(!uu::desktopModeRpc(1, 14, 2280, 1428, 120, 6, 60).empty());
    for (auto bad : {+[] { uu::desktopModeRpc(1, 14, 2281, 1428, 120, 3); }, +[] { uu::desktopModeRpc(1, -1, 2280, 1428, 120, 3); },
                     +[] { uu::desktopModeRpc(1, 14, 2280, 1428, 90, 3); }, +[] { uu::desktopModeRpc(1, 14, 2280, 1428, 120, 6, 0); },
                     +[] { uu::selectScreenRpc(0, 5, 60, 3, true, 1920, 1080); }, +[] { uu::selectScreenRpc(1, -1, 60, 3, true, 1920, 1080); },
                     +[] { uu::selectScreenRpc(1, 5, 60, 6, true, 1920, 1080); },
                     +[] { uu::quitSuperScreenRpc(0); }, +[] { uu::enterSuperScreenRpc(0, 2280, 1428); },
                     +[] { uu::enterSuperScreenRpc(1, 2281, 1428); }}) {
        threw = false;
        try { bad(); } catch (const std::invalid_argument &) { threw = true; }
        assert(threw);
    }
    std::puts("protocol_test ok");
}

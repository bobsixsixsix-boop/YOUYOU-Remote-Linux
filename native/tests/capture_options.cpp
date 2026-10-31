#include "capture_rate.hpp"
#include "stream_size.hpp"
#include "config_store.hpp"
#include "protocol.hpp"
#include "quality_rpc.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>

static void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

struct TemporaryConfig {
    std::filesystem::path previous=std::filesystem::current_path(), directory;
    TemporaryConfig() {
        char path[]="/tmp/youyou-capture-options-XXXXXX";
        check(mkdtemp(path)!=nullptr,"temporary config directory");
        directory=path;
        std::filesystem::current_path(directory);
    }
    ~TemporaryConfig() {
        std::error_code ec;
        std::filesystem::current_path(previous,ec);
        std::filesystem::remove_all(directory,ec);
    }
};

int main() {
    try {
        using namespace uu;
        // Captured schema from GameViewer 4.44. The legacy tier is 144 (4).
        // The independently encoded fpsCount and DecoderCap.fps are both 120.
        const Bytes expectedConnect{
            0x08,1, 0x10,255,255,255,255,255,255,255,255,255,1,
            0x1a,20, 0x08,4, 0x10,3, 0x18,1, 0x20,3,
            0x2a,6, 0x08,0x80,0x0a,0x10,0xd0,0x05,
            0x50,0, 0x58,120,
            0x22,10, 0x08,120, 0x10,2, 0x18,0x80,0x0a, 0x20,0xd0,0x05,
            0x22,10, 0x08,120, 0x10,1, 0x18,0x80,0x0a, 0x20,0xd0,0x05,
            0x32,8,0x08,0x80,0x0a,0x10,0xd0,0x05,0x18,120,
            0x3a,6,0x08,0x80,0x0a,0x10,0xd0,0x05,0x40,2
        };
        check(connectOptions(1280,720,120,3,0,false,true)==expectedConnect, "ConnectOptions wire values");
        const Bytes rpc=qualityUpdateRpc(1,120,3,0);
        const Bytes body{0x08,4,0x10,3,0x18,1,0x60,0,0x90,1,120,0x20,0};
        check(rpc.size()>=body.size() && std::equal(body.rbegin(),body.rend(),rpc.rbegin()),
              "CaptureSettingRequest wire values");
        const auto scale=videoScaleRpc(2,2560,1440,1600,900,120,3,0);
        const Bytes videoBounds{0x08,4,0x10,3,0x18,1,0x60,0,0x90,1,120,0x20,0,0x50,3,
            0x70,0xc0,0x0c,0x78,0x84,0x07,0x80,1,0xc0,0x0c,0x88,1,0x84,0x07};
        check(scale.size()>=videoBounds.size() && std::equal(videoBounds.rbegin(),videoBounds.rend(),scale.rbegin()),
              "video bound RPC must leave physical display dimensions unset");
        check(streamSize(3040,1904) && streamSize(1904,3040) && !streamSize(1727,972) && !streamSize(4096,4096),
              "custom resolution validation");
        bool rejected=false;
        try { fpsSettingRpc(1,120); } catch(const std::invalid_argument &) { rejected=true; }
        check(rejected, "enum-only RPC must reject 120");
        const Bytes sync{0xca,1,2,0x08,4};
        auto parsed=parseCaptureSettingSync(sync.data(),sync.size());
        check(parsed && fpsFromTier(parsed->fps)==144, "144 tier was misreported as 120");
        TemporaryConfig temporary;
        auto &store=ConfigStore::instance();
        store.reload();
        check(!store.has("vsync") && !store.verticalSync(), "missing vertical sync preference must default off");
        saveQualityConfig(1280,720,120,3,false,8,0,false,true);
        store.reload();
        check(store.value("vsync")=="0" && !store.verticalSync(), "vertical sync off round trip");
        check(store.value("video_codec")=="av1" && store.integer("fps",60,30,120)==120, "AV1 preference round trip");
        saveQualityConfig(1280,720,60,3,true,8,0,true);
        store.reload();
        check(store.value("vsync")=="1" && store.verticalSync(), "explicit vertical sync on round trip");
        check(store.value("video_codec")=="hevc" && store.integer("fps",120,30,120)==60, "legacy preference round trip");
        saveQualityConfig(1904,3040,120,3,false,8,0,false,true);
        store.reload();
        check(store.value("width")=="1904" && store.value("height")=="3040","portrait video size round trip");
        return 0;
    } catch(const std::exception &e) {
        std::fprintf(stderr,"%s\n",e.what());
        return 1;
    }
}

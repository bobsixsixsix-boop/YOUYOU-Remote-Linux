#include "capture_rate.hpp"
#include "stream_size.hpp"
#include "config_store.hpp"
#include "protocol.hpp"
#include "quality_rpc.hpp"
#include "quality_confirmation.hpp"
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

static void highRateWire(int fps, uint8_t rateLow) {
    using namespace uu;
    check(supportedCaptureRate(fps) && captureRateTier(fps)==4, "high rate must use tier 4");
    // Both rates cross the one-byte varint boundary. These are independent
    // golden wire bytes, including every enclosing length, for 144 / 164.
    const Bytes expected{
        0x08,1, 0x10,255,255,255,255,255,255,255,255,255,1,
        0x1a,21, 0x08,4, 0x10,3, 0x18,1, 0x20,3,
        0x2a,6, 0x08,0x80,0x0a,0x10,0xd0,0x05,
        0x50,0, 0x58,rateLow,1,
        0x22,11, 0x08,rateLow,1, 0x10,2, 0x18,0x80,0x0a, 0x20,0xd0,0x05,
        0x22,11, 0x08,rateLow,1, 0x10,1, 0x18,0x80,0x0a, 0x20,0xd0,0x05,
        0x32,9,0x08,0x80,0x0a,0x10,0xd0,0x05,0x18,rateLow,1,
        0x3a,6,0x08,0x80,0x0a,0x10,0xd0,0x05,0x40,2
    };
    check(connectOptions(1280,720,fps,3,0,false,true)==expected,
          "high-rate ConnectOptions lost exact capture / decoder / virtual cadence");
    const auto rpc=qualityUpdateRpc(1,fps,3,0);
    const Bytes body{0x08,4,0x10,3,0x18,1,0x60,0,0x90,1,rateLow,1,0x20,0};
    check(rpc.size()>=body.size() && std::equal(body.rbegin(),body.rend(),rpc.rbegin()),
          "online high-rate request lost exact fpsCount");
    const auto scale=videoScaleRpc(2,2560,1440,1600,900,fps,3,0);
    const Bytes bounds{0x08,4,0x10,3,0x18,1,0x60,0,0x90,1,rateLow,1,0x20,0,0x50,3,
        0x70,0xc0,0x0c,0x78,0x84,0x07,0x80,1,0xc0,0x0c,0x88,1,0x84,0x07};
    check(scale.size()>=bounds.size() && std::equal(bounds.rbegin(),bounds.rend(),scale.rbegin()),
          "high-rate video scale must preserve cadence and leave physical dimensions unset");
}

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
        highRateWire(144,0x90);
        highRateWire(164,0xa4);
        check(maxCaptureFps==164, "capture maximum must include the offered 164 rate");
        check(capturePlayoutFrameBudget(30)==8 && capturePlayoutFrameBudget(60)==8 &&
              capturePlayoutFrameBudget(120)==14 && capturePlayoutFrameBudget(144)==17 &&
              capturePlayoutFrameBudget(164)==19, "playout frame budget did not scale with high cadence");
        for(int fps : {144,164}) {
            const auto levels=streamLevels(1920,1080,fps);
            check(std::string(levels.avcHex)=="34" && levels.hevc==153 && levels.av1==13,
                  "high-rate 1080p offer retained insufficient legacy codec levels");
            const auto maximum=streamLevels(4096,2160,fps);
            check(std::string(maximum.avcHex)=="3d" && maximum.hevc==183 && maximum.av1==17,
                  "largest high-rate offer exceeds the advertised codec level");
        }
        const auto legacyLevels=streamLevels(1920,1080,120);
        check(std::string(legacyLevels.avcHex)=="1f" && legacyLevels.hevc==123 && legacyLevels.av1==13,
              "existing 1080p offers changed unexpectedly");
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
        for(int fps : {120,144,164}) {
            check(!enumOnlyCaptureRate(fps), "high cadence admitted to enum-only request");
            bool rejected=false;
            try { fpsSettingRpc(1,fps); } catch(const std::invalid_argument &) { rejected=true; }
            check(rejected, "enum-only RPC must reject exact high frame rates");
        }
        check(!fpsSettingRpc(1,30).empty() && !fpsSettingRpc(1,60).empty(), "legacy enum-only RPC compatibility");
        check(enumOnlyCaptureRate(30) && enumOnlyCaptureRate(60), "legacy enum helper compatibility");
        for(int fps : {0,29,61,90,121,143,145,163,165,240}) {
            check(!supportedCaptureRate(fps), "unsupported rate admitted");
            bool rejected=false;
            try { captureRateTier(fps); } catch(const std::invalid_argument &) { rejected=true; }
            check(rejected, "unsupported rate silently mapped to a tier");
        }
        const Bytes sync{0xca,1,2,0x08,4};
        auto parsed=parseCaptureSettingSync(sync.data(),sync.size());
        check(parsed && fpsFromTier(parsed->fps)==144, "144 tier was misreported as 120");
        check(captureRateSyncEvidence(144,parsed->fps)==144 &&
              captureRateSyncEvidence(120,parsed->fps)==0 && captureRateSyncEvidence(164,parsed->fps)==0,
              "ambiguous 144 capability tier became exact 120 / 164 evidence");
        check(captureRateSyncEvidence(164,2)==60 && captureRateSyncEvidence(164,1)==30 &&
              captureRateSyncEvidence(164,0)==0 && captureRateSyncEvidence(164,99)==0,
              "capture sync lost host fallback rate or accepted an unknown tier");
        QualityApplyTracker confirmation;
        confirmation.beginRequest();
        confirmation.armConfirmation(3);
        confirmation.acknowledge();
        confirmation.evidence.observeFps(fpsFromTier(parsed->fps));
        confirmation.evidence.observeQuality(3);
        confirmation.evidence.observeSize(1280,720);
        for(int fps : {120,164}) {
            check(!confirmation.evaluate({1280,720,fps,3,0}) && confirmation.pending,
                  "tier 144 falsely confirmed a different exact frame rate");
        }
        check(confirmation.evaluate({1280,720,144,3,0}), "tier 144 cannot confirm actual 144 request");
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
        for(int fps : {144,164}) {
            saveQualityConfig(3040,1904,fps,6,false,0,100,false,true);
            store.reload();
            check(store.value("fps")==std::to_string(fps) && store.integer("fps",60,30,164)==fps,
                  "high-rate config fell back after save / reload");
            check(store.value("video_codec")=="av1" && !store.verticalSync(),
                  "high-rate save changed codec or vertical sync preference");
        }
        return 0;
    } catch(const std::exception &e) {
        std::fprintf(stderr,"%s\n",e.what());
        return 1;
    }
}

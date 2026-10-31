#include "bridge.h"
#include "client.hpp"
#include "background.hpp"
#include "config_store.hpp"
#include "diagnostics.hpp"
#include "rtc_diagnostics.hpp"
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace {
thread_local std::string lastError;
// SDL audio initialization is allowed to fail when a desktop has no audio
// device. Media reports that state and can still present the remote screen.
struct Platform {
    bool audio = false;
    Platform() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("libcurl initialization failed");
        audio = SDL_InitSubSystem(SDL_INIT_AUDIO) == 0;
    }
    ~Platform() {
        if (audio) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        curl_global_cleanup();
    }
};
struct Core {
    Platform platform;
    std::shared_ptr<uu::Media> media;
    std::unique_ptr<uu::Client> client;
    uu::Picture picture;
    std::string snapshot;
    Core(const char *key, const char *id) {
        uu::ConfigStore::instance().reload();
        rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel, const std::string &line) {
            const auto safe = uu::rtcLogEvent(line);
            if (!safe.empty()) uu::diagnostic(safe.c_str());
        });
        media = std::make_shared<uu::Media>();
        media->setYuvOutput(true);
        client = std::make_unique<uu::Client>(key, id, media);
    }
    ~Core() {
        client.reset();
        rtc::Cleanup().wait();
        uu::waitBackgroundTasks();
        media.reset();
        uu::flushDiagnosticsLog();
    }
};
Core &handle(void *value) {
    if (!value) throw std::invalid_argument("Client is not open");
    return *static_cast<Core *>(value);
}
void saveError() noexcept {
    try { throw; }
    catch (const std::exception &ex) { lastError = ex.what(); }
    catch (...) { lastError = "Native client failed"; }
}
}

extern "C" const char *yy_error() { return lastError.c_str(); }

// The ported upstream SDL overlay shares the same authenticated session and decoder.
uu::Client &yy_ui_client(void *core) { return *handle(core).client; }
std::shared_ptr<uu::Media> yy_ui_media(void *core) { return handle(core).media; }

extern "C" void *yy_open(const char *key, const char *id) {
    try {
        if (!key || std::strlen(key) != 24 || !id || std::strlen(id) != 36)
            throw std::invalid_argument("Invalid signing key or client ID length");
        return new Core(key, id);
    } catch (...) { saveError(); return nullptr; }
}
extern "C" void yy_close(void *value) { delete static_cast<Core *>(value); }

extern "C" int yy_command(void *value, const char *name, const char *json) {
    try {
        if (!name || !json) throw std::invalid_argument("Missing command or argument");
        handle(value).client->command(name, uu::Json::parse(json));
        return 0;
    } catch (...) { saveError(); return -1; }
}

extern "C" const char *yy_snapshot(void *value) {
    try {
        auto &core = handle(value);
        const auto s = core.client->snapshot();
        const auto m = core.media->stats();
        const auto t = core.client->streamStats();
        core.snapshot = uu::Json{
            {"stage", s.stage}, {"detail", s.detail}, {"qrUrl", s.qrUrl},
            {"nickname", s.nickname}, {"authenticated", s.authenticated},
            {"streaming", s.streaming}, {"devices", s.devices}, {"loginMode", s.loginMode},
            {"width", s.width}, {"height", s.height}, {"fps", s.fps}, {"hevc", s.hevc}, {"av1", s.av1},
            {"videoCodecPreference",s.av1?"av1-preferred":s.hevc?"hevc":"h264"},{"decodedCodec",m.codec},{"vsync",s.vsync},
            {"quality", s.quality}, {"qualityNotice", s.qualityNotice},
            {"controlChannelReady", s.controlChannelReady}, {"activeDeviceId", s.activeDeviceId},
            {"textChannelReady", s.textChannelReady}, {"textNotice", s.textNotice},
            {"appliedWidth",s.appliedWidth}, {"appliedHeight",s.appliedHeight},
            {"decoder", m.decoder}, {"decodeMs", m.decodeMs}, {"decodeFps", m.fps},
            {"publishedTotal", m.publishedTotal}, {"mediaStatus", core.media->status()},
            {"decodedWidth", m.width}, {"decodedHeight", m.height},
            {"decodeErrors", m.decodeErrors}, {"transport", t.transport},
            {"rttMs", t.rttMs}, {"bitrateMbps", t.video.bitrateMbps}
        }.dump(-1, ' ', false, uu::Json::error_handler_t::replace);
        return core.snapshot.c_str();
    } catch (...) { saveError(); return nullptr; }
}

extern "C" int yy_frame(void *value, uint64_t after, YYFrame *out) {
    try {
        if (!out) throw std::invalid_argument("Missing frame output");
        *out = {};
        auto &core = handle(value);
        core.picture = core.media->picture(after);
        const auto &p = core.picture;
        if (!p.pixels || !p.serial) return 0;
        int32_t format = 0;
        if (p.pixelFormat == SDL_PIXELFORMAT_NV12)
            format = p.yuvMode == SDL_YUV_CONVERSION_BT709 ? 2 : 1;
        *out = {p.pixels->data(), p.pixels->size(), p.serial, p.width, p.height, format};
        return 1;
    } catch (...) { saveError(); return -1; }
}

// Link-time interception measures successful submissions to this library's
// SDL audio device, including errors or absent output during the offline test.
static std::atomic<uint64_t> audioBytes{0};
extern "C" int __real_SDL_QueueAudio(SDL_AudioDeviceID, const void *, Uint32);
extern "C" int __wrap_SDL_QueueAudio(SDL_AudioDeviceID id, const void *data, Uint32 len) {
    const int result = __real_SDL_QueueAudio(id, data, len);
    if (result == 0) audioBytes.fetch_add(len, std::memory_order_relaxed);
    return result;
}

extern "C" int yy_test_media() {
    try {
        Platform platform;
        uu::Media media;
        constexpr uint8_t red[] = {
#include "test_frame.inc"
        };
        constexpr uint8_t opusSilence[] = {0xf8, 0xff, 0xfe};
        const uint64_t epoch = media.generation();
        const auto before = audioBytes.load();
        media.audio(opusSilence, sizeof(opusSilence), epoch);
        if (audioBytes.load() < before + 960 * 4)
            throw std::runtime_error("Opus decoding or SDL stereo playback failed");
        // Submit repeated IDRs to service FFmpeg frame-threading delay.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            media.video(red, sizeof(red), epoch);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            const auto frame = media.picture(0);
            if (frame.width == 16 && frame.height == 16 && frame.pixels &&
                frame.pixels->size() == 16 * 16 * 4 && frame.pixels->at(0) > 200 &&
                frame.pixels->at(1) < 30 && frame.pixels->at(2) < 30)
                return 0;
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("H.264 red fixture did not produce the expected RGBA frame");
    } catch (...) { saveError(); return -1; }
}

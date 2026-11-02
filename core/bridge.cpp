// SPDX-License-Identifier: GPL-3.0-or-later
// C ABI over upstream's uu::Client (bridge.h). Init and teardown order follow
// upstream main.cpp. Upstream keeps config.ini, session.json (login tokens) and
// connection.log in the working directory, so the player chdirs into its 0700
// state directory before yy_open. The frame calls live in media_linux.cpp.
#include "bridge.h"
#include "background.hpp"
#include "client.hpp"
#include "config_store.hpp"
#include "diagnostics.hpp"
#include "rtc_diagnostics.hpp"
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {
thread_local std::string lastError;
std::mutex coreMutex;

// A desktop without an audio device still streams video; Media reports it.
struct Platform {
    bool audio = false;
    Platform() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("libcurl initialization failed");
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
    Core(const char *key, const char *id) {
        uu::ConfigStore::instance().reload();
        rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel, const std::string &line) {
            const auto event = uu::rtcLogEvent(line);
            if (!event.empty()) uu::diagnostic(event.c_str());
        });
        media = std::make_shared<uu::Media>();
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
std::unique_ptr<Core> core;

Core &open() {
    if (!core) throw std::logic_error("yy_open has not succeeded");
    return *core;
}
int failed() noexcept {
    try {
        throw;
    } catch (const std::exception &e) {
        lastError = e.what();
    } catch (...) {
        lastError = "native core failed";
    }
    return -1;
}
} // namespace

extern "C" const char *yy_error(void) { return lastError.c_str(); }

extern "C" int yy_open(const char *key, const char *client_id) {
    try {
        if (!key || std::strlen(key) != 24 || !client_id || std::strlen(client_id) != 36)
            throw std::invalid_argument("signing key must be 24 bytes and client ID 36 characters");
        std::lock_guard lock(coreMutex);
        if (core) throw std::logic_error("a client is already open");
        core = std::make_unique<Core>(key, client_id);
        return 0;
    } catch (...) {
        return failed();
    }
}

extern "C" void yy_close(void) {
    std::lock_guard lock(coreMutex);
    core.reset();
}

extern "C" int yy_command(const char *name, const char *json) {
    try {
        if (!name || !json) throw std::invalid_argument("command name and JSON argument are required");
        auto data = uu::Json::parse(json);
        std::lock_guard lock(coreMutex);
        open().client->command(name, std::move(data));
        return 0;
    } catch (...) {
        return failed();
    }
}

extern "C" char *yy_snapshot(void) {
    try {
        std::lock_guard lock(coreMutex);
        auto &c = open();
        const auto s = c.client->snapshot();
        const auto m = c.media->stats();
        const auto t = c.client->streamStats();
        const auto text = uu::Json{
            {"stage", s.stage}, {"detail", s.detail}, {"qrUrl", s.qrUrl}, {"nickname", s.nickname},
            {"loginMode", s.loginMode}, {"smsRetryAt", s.smsRetryAt},
            {"authenticated", s.authenticated}, {"streaming", s.streaming}, {"devices", s.devices},
            {"activeDeviceId", s.activeDeviceId},
            {"controlChannelReady", s.controlChannelReady}, {"textChannelReady", s.textChannelReady},
            {"width", s.width}, {"height", s.height}, {"fps", s.fps}, {"quality", s.quality},
            {"maxBitrateMbps", s.maxBitrateMbps}, {"vsync", s.vsync}, {"hevc", s.hevc},
            {"playoutDelayMs", s.playoutDelayMs}, {"qualityPending", s.qualityPending},
            {"qualityNotice", s.qualityNotice}, {"videoScaleNotice", s.videoScaleNotice},
            {"applied", {{"valid", s.appliedValid}, {"fps", s.appliedFpsKnown ? s.appliedFps : 0},
                         {"quality", s.appliedQualityKnown ? s.appliedQuality : 0},
                         {"width", s.appliedWidthKnown ? s.appliedWidth : 0},
                         {"height", s.appliedHeightKnown ? s.appliedHeight : 0},
                         {"bitrateMbps", s.appliedBitrateKnown ? s.appliedBitrateMbps : 0}}},
            {"setting", {{"accepted", s.settingAccepted}, {"applied", s.settingApplied},
                         {"rejected", s.settingRejected}, {"applyPending", s.settingApplyPending},
                         {"errorCode", s.settingErrorCode}, {"source", s.settingSource}}},
            {"server", {{"valid", s.serverCapabilityValid}, {"display", s.serverDisplayInfo},
                        {"codec", s.serverCodecInfo}, {"capture", s.serverCaptureImpl},
                        {"encoder", s.serverEncoderImpl}, {"videoTrack", s.serverVideoTrack}}},
            // PrivacyMode: 0 unknown, 1 off, 2 normal, 3 screen off (same numbers privacy-screen takes).
            {"privacy", {{"known", s.privacyKnown}, {"pending", s.privacyPending},
                         {"mode", unsigned(s.privacyMode)}, {"requested", unsigned(s.privacyRequestedMode)},
                         {"notice", s.privacyNotice}}},
            {"desktopScale", {{"virtual", s.desktopVirtual}, {"current", s.desktopScale},
                              {"recommended", s.desktopScaleRecommended}, {"options", s.desktopScales},
                              {"requested", s.desktopScaleRequested}, {"notice", s.desktopScaleNotice}}},
            {"screens", {{"list", s.screens}, {"current", s.screenCurrent},
                         {"requested", s.screenRequested}, {"notice", s.screenNotice}}},
            {"text", {{"pending", s.textPending}, {"revision", s.textRevision}, {"result", s.textResult},
                      {"notice", s.textNotice}}},
            {"media", {{"status", c.media->status()}, {"decoder", m.decoder}, {"codec", m.codec},
                       {"width", m.width}, {"height", m.height}, {"fps", m.fps}, {"decodeMs", m.decodeMs},
                       {"decoded", m.decodedTotal}, {"errors", m.decodeErrors}, {"dropped", m.droppedFrames},
                       {"keyframes", m.keyframeCount}}},
            {"network", {{"transport", t.transport}, {"rttMs", t.rttMs},
                         {"bitrateMbps", t.video.bitrateMbps}, {"lossPercent", t.video.lossPercent},
                         {"jitterMs", t.video.jitterMs}}},
        }.dump(-1, ' ', false, uu::Json::error_handler_t::replace);
        return strdup(text.c_str());
    } catch (...) {
        failed();
        return nullptr;
    }
}

extern "C" void yy_free(char *text) { std::free(text); }

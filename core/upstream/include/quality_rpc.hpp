#pragma once
#include "protocol.hpp"
#include <optional>
namespace uu {
// Main.Message.simple_action belongs to CONTROL, not the clipboard/RPC TEXT channel.
inline constexpr const char *systemActionChannel="CONTROL_DATA_CHANNEL";
struct CaptureRpcAck {uint64_t requestId=0;unsigned errors=0,entries=0;int errorCode=0;bool capture=false;unsigned otherFields=0;bool superScreen=false;};
// One screen of Main.screens, in the host's order, for switching monitors (fps = Screen field 2).
struct ScreenEntry {int id=-1,type=0,width=0,height=0,fps=0;};
// The current screen of Main.screens; type 1 is a virtual display, modes are
// the desktop sizes it offers, scales its Windows display scales (Screen.dpi_scale).
// `all` is every screen, the current one included.
struct ScreenSourceInfo {int screenId=-1;unsigned count=0;int type=0,width=0,height=0;std::vector<std::pair<int,int>> modes;
                        int scale=0,recommendedScale=0;std::vector<int> scales;std::vector<ScreenEntry> all;
                        bool currentOmitted=false;}; // no current ID on the wire (protobuf drops a 0)
struct CaptureSettingSyncInfo {
    unsigned fields=0; // bit0 fps, bit1 quality, bit2 width, bit3 height, bit4 bitrate
    int fps=0,quality=0,chroma=0,resolutionType=0,dpiScale=0,orientation=0;
    int64_t maxCustomBitrate=0;
    int width=0,height=0,pixelWidth=0,pixelHeight=0;
};
std::optional<CaptureSettingSyncInfo> parseCaptureSettingSync(const uint8_t *data,size_t size) noexcept;
struct TextRpcAck {uint64_t requestId=0;unsigned status=0;};
enum class PrivacyMode : unsigned { Unknown=0, Off=1, Normal=2, ScreenOff=3 };
struct PrivacyRpcAck {uint64_t requestId=0;int errorCode=0;PrivacyMode requestedMode=PrivacyMode::Unknown,currentMode=PrivacyMode::Unknown;};
Bytes privacyScreenRpc(uint64_t requestId,PrivacyMode mode);
std::optional<PrivacyRpcAck> parsePrivacyRpcAck(const uint8_t *data,size_t size) noexcept;
Bytes clipboardTextRpc(uint64_t requestId,const std::string &text);
std::optional<TextRpcAck> parseTextRpcAck(const uint8_t *data,size_t size) noexcept;
std::optional<ScreenSourceInfo> parseScreenSources(const uint8_t *data,size_t size) noexcept;
// Diagnostic-only bounded protobuf field summary. It never includes payload
// strings; used to locate the host screen C7 width/height/pixel fields.
std::string screenSourceWireSummary(const uint8_t *data,size_t size) noexcept;
Bytes nativeTouchMessage(uint64_t seq,int type,const Json &points);
Bytes simpleActionMessage(uint64_t seq,unsigned action,std::optional<bool> enabled=std::nullopt);
Bytes captureSettingsRpc(uint64_t requestId,int width,int height,int fps,int quality,int screenId,int customMbps=0);
// Selects a virtual display with a new desktop size from its modes and the
// matching encoder size (v0.1.15 virtual_display "Initialize", GameViewer 4.44).
Bytes desktopModeRpc(uint64_t requestId,int screenId,int width,int height,int fps,int quality,int customMbps=0);
// Streams another of the host's screens with the CaptureSettingRequest the
// official app (uuyc 4.45 ScreenViewModel) sends: ChooseType_DEFAULT with -1/-1
// so the host sizes the stream, codec_type matching our track, our decoder max.
Bytes selectScreenRpc(uint64_t requestId,int screenId,int fps,int quality,bool hevc,int maxWidth,int maxHeight,int customMbps=0);
// Leaves GameViewer's super screen (an empty QuitSuperScreenRequest, RpcRequest field 14):
// the host takes down the virtual display it put in place of its desktop.
Bytes quitSuperScreenRpc(uint64_t requestId);
// Puts the super screen back at this size (EnterSuperScreenRequest, RpcRequest
// field 26: reason MANUAL, resolution), as the official app's 超级屏 does.
Bytes enterSuperScreenRpc(uint64_t requestId,int width,int height);
// Protocol-native settings: Main.RpcRequest field3/4 with their dedicated
// FpsSettingRequest / VideoQualitySettingReqeust bodies.
Bytes fpsSettingRpc(uint64_t requestId,int fps);
Bytes videoQualitySettingRpc(uint64_t requestId,int quality);
// Quality/fps/bitrate-only update. Deliberately omits resolution fields so a
// bitrate or frame-rate change cannot switch the host capture resolution.
// dpiScale, when set, also changes the screen's Windows display scale (field 9).
Bytes qualityUpdateRpc(uint64_t requestId,int fps,int quality,int screenId,int customMbps=0,int dpiScale=0);
// Full mobile-shaped capture request: source dimensions stay as observed, while
// the pixel dimensions bound the encoded video. Keeps fps/quality/bitrate.
Bytes videoScaleRpc(uint64_t requestId,int sourceWidth,int sourceHeight,
                    int outputWidth,int outputHeight,int fps,int quality,
                    int screenId,int customMbps=0);
std::optional<CaptureRpcAck> parseCaptureRpcAck(const uint8_t *data,size_t size) noexcept;
}

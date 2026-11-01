#include "quality_rpc.hpp"
#include <iostream>

int main() {
    using uu::Bytes;
    using uu::PrivacyMode;
    const auto state = [](Bytes b) { return uu::parsePrivacyScreenState(b.data(), b.size()); };

    // Message.system_state_change (15) -> private_screen_state (5).
    const auto fallback = state({0x7a, 6, 0x2a, 4, 0x10, 3, 0x18, 2});
    if (!fallback || fallback->requestId || fallback->requestedMode != PrivacyMode::ScreenOff ||
        fallback->currentMode != PrivacyMode::Normal) return 1;
    const auto error = state({0x7a, 4, 0x2a, 2, 0x08, 5});
    if (!error || error->errorCode != 5 || error->currentMode != PrivacyMode::Unknown) return 2;
    const auto echo = state({0x7a, 4, 0x2a, 2, 0x10, 2});
    if (!echo || echo->currentMode != PrivacyMode::Unknown) return 3;
    // Host second-screen mode rejects all privacy choices with 508 while
    // separately reporting that current display mode is Off. Never promote
    // the requested mode to the authoritative current mode on this failure.
    for(const auto requested : {PrivacyMode::Off,PrivacyMode::Normal,PrivacyMode::ScreenOff}) {
        const Bytes bytes={0xb2,0x01,13,0x0a,2,0x08,7,0x1a,7,
                           0x08,0xfc,0x03,0x10,uint8_t(requested),0x18,1};
        const auto rejected=uu::parsePrivacyRpcAck(bytes.data(),bytes.size());
        if(!rejected || rejected->requestId!=7 || rejected->errorCode!=508 ||
           rejected->requestedMode!=requested || rejected->currentMode!=PrivacyMode::Off)return 4;
    }
    const auto rejectedPush=state({0x7a,9,0x2a,7,0x08,0xfc,0x03,0x10,2,0x18,1});
    if(!rejectedPush || rejectedPush->errorCode!=508 ||
       rejectedPush->requestedMode!=PrivacyMode::Normal || rejectedPush->currentMode!=PrivacyMode::Off)return 5;
    for (const Bytes &invalid : std::initializer_list<Bytes>{
             {0x7a, 4, 0x2a, 2, 0x18, 4},          // future enum value
             {0x7a, 6, 0x2a, 4, 0x18, 2, 0x18, 1}, // duplicate current mode
             {0x7a, 4, 0x2a, 2, 0x18},             // truncated value
             {0x7a, 4, 0x1a, 2, 0x18, 2}}) {        // another type of system change
        if (state(invalid)) return 3;
    }
    std::cout << "PASS privacy state notifications\n";
}

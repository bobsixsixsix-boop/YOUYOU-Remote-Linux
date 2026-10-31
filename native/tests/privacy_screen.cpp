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
    for (const Bytes &invalid : std::initializer_list<Bytes>{
             {0x7a, 4, 0x2a, 2, 0x18, 4},          // future enum value
             {0x7a, 6, 0x2a, 4, 0x18, 2, 0x18, 1}, // duplicate current mode
             {0x7a, 4, 0x2a, 2, 0x18},             // truncated value
             {0x7a, 4, 0x1a, 2, 0x18, 2}}) {        // another type of system change
        if (state(invalid)) return 3;
    }
    std::cout << "PASS privacy state notifications\n";
}

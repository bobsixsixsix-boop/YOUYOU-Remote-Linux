// SPDX-License-Identifier: GPL-3.0-or-later
#include "decode_catchup.hpp"
#include <iostream>
#include <stdexcept>

void check(bool condition) {
    if (!condition) throw std::runtime_error("Incorrect catch-up decision");
}

int main() {
    using uu::deferDecodedOutput;
    check(!deferDecodedOutput(16, 200, 120, 1000, -1));
    check(!deferDecodedOutput(0, 200, 120, 1000, 999));
    check(!deferDecodedOutput(1, 2, 120, 1000, 999));
    check(!deferDecodedOutput(2, 16, 30, 1000, 999));
    check(deferDecodedOutput(1, 19, 120, 1000, 990));
    check(deferDecodedOutput(3, 0, 120, 1000, 990));
    check(deferDecodedOutput(16, 200, 120, 1032, 1000));
    check(!deferDecodedOutput(16, 200, 120, 1033, 1000));
    check(!deferDecodedOutput(16, 200, 120, 1000, 1001));
    int last = -1, shown = 0, skipped = 0;
    for (int frame = 0; frame < 25; ++frame) {
        int now = frame * 2;
        if (deferDecodedOutput(24 - frame, 200, 120, now, last)) {
            ++skipped;
        } else {
            last = now;
            ++shown;
        }
    }
    check(last == 48 && shown == 3 && skipped == 22);
    std::cout << "First frame, periodic output, drained tail and idle cadence passed.\n";
}

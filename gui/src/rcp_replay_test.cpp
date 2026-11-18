// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "rcp/replay_schedule.h"

#include <array>
#include <iostream>

int main()
{
    using namespace rcp;
    constexpr ReplayTiming timing{6 * secondNs, 2 * secondNs, 80 * msNs, 0};
    static_assert(timing.due(1919 * msNs) == 1919 * msNs);
    static_assert(timing.due(1920 * msNs) == 2 * secondNs);
    static_assert(timing.due(2 * secondNs) == 2 * secondNs);
    static_assert(timing.due(5980 * msNs) == 6 * secondNs);
    constexpr ReplayTiming stall{6 * secondNs, 0, 0, 550 * msNs};
    static_assert(stall.due(3000 * msNs) == 3550 * msNs);
    static_assert(stall.due(3551 * msNs) == 3551 * msNs);

    std::array<unsigned int, 2> dropped{};
    for (const auto rate : {60, 100, 40}) {
        int held = 0;
        auto previous = std::int64_t{0};
        for (int i = 0; i < rate * 6; ++i) {
            const auto nominal = timing.nominal(i, rate);
            const auto due = timing.due(nominal);
            if (due < nominal || due < previous || due > timing.duration_ns)
                return 1;
            held += due > nominal;
            previous = due;
        }
        if (!held)
            return 1;
    }
    for (unsigned int i = 1; i < 600; ++i) {
        dropped[0] += replayDrop(2406, 1, i, 3);
        dropped[1] += replayDrop(2406, 2, i, 3);
        if (replayDrop(2406, 2, i, 0) || !replayDrop(2406, 2, i, 100))
            return 1;
    }
    if (!dropped[0] || !dropped[1] || dropped[0] == dropped[1])
        return 1;
    std::cout << "Replay schedule passed: video/audio/input deadlines, stall, seed and separate loss lanes\n";
    return 0;
}

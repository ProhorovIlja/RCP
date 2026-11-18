// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <algorithm>
#include <cstdint>

namespace rcp {
constexpr std::int64_t secondNs = 1000000000;
constexpr std::int64_t msNs = 1000000;

struct ReplayTiming {
    std::int64_t duration_ns = 0;
    std::int64_t burst_period_ns = 0;
    std::int64_t burst_hold_ns = 0;
    std::int64_t stall_ns = 0;

    constexpr std::int64_t nominal(int slot, int rate) const
    {
        return std::int64_t(slot) * secondNs / rate;
    }

    constexpr std::int64_t due(std::int64_t time) const
    {
        // Delay only packets that are already due. Never send future video or
        // audio early to manufacture a burst.
        if (burst_period_ns > 0 && burst_hold_ns > 0) {
            const auto hold = (std::min)(burst_hold_ns, burst_period_ns);
            const auto phase = time % burst_period_ns;
            if (phase >= burst_period_ns - hold)
                time += burst_period_ns - phase;
        }
        if (stall_ns > 0 && time >= duration_ns / 2 && time < duration_ns / 2 + stall_ns)
            time = duration_ns / 2 + stall_ns;
        return (std::min)(time, duration_ns);
    }
};

// A stateless mixer makes loss decisions independent of runner timing and of
// other streams, so toggling a playback backend cannot change the test trace.
constexpr std::uint32_t replaySample(std::uint32_t seed, std::uint32_t lane, std::uint32_t slot)
{
    std::uint32_t value = seed ^ (lane * 0x9e3779b9u) ^ (slot * 0x85ebca6bu);
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

constexpr bool replayDrop(std::uint32_t seed, std::uint32_t lane, std::uint32_t slot, int percent)
{
    return percent > 0 && replaySample(seed, lane, slot) % 100u < static_cast<unsigned int>(percent);
}
}

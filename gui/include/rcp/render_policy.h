// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <algorithm>

namespace rcp {
constexpr bool useFrameMixing(bool low_latency, bool custom_mixer_off)
{
    return !low_latency && !custom_mixer_off;
}

constexpr bool expeditedFrameTimerNeeded(bool timer_active, int remaining_ms)
{
    return !timer_active || remaining_ms > 0;
}

inline int renderQueueLimit(int configured, bool low_latency, bool pending, double average)
{
    // A two-frame cap preserves the render path's minimum while preventing extra buffering.
    if (low_latency)
        return 2;
    const int depth = (std::max)(configured, 2);
    return depth + (pending || average >= depth - 0.25 ? 1 : 0);
}
}

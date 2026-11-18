// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include <algorithm>
#include <cstddef>

namespace rcp {
constexpr double bufferedAudioMs(std::size_t bytes, std::size_t frame_bytes, unsigned int sample_rate)
{
    if (!frame_bytes || !sample_rate)
        return -1.0;
    return (1000.0 * bytes / frame_bytes) / sample_rate;
}

constexpr std::size_t wholePcmFrames(std::size_t bytes, std::size_t frame_bytes)
{
    return frame_bytes ? bytes - bytes % frame_bytes : 0;
}

constexpr std::size_t audioQueueChunk(std::size_t target, std::size_t queued,
    std::size_t pending, std::size_t contiguous, std::size_t frame_bytes)
{
    if (queued >= target)
        return 0;
    return wholePcmFrames((std::min)({target - queued, pending, contiguous}), frame_bytes);
}
}

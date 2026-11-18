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

// The DualSense haptic output uses 48 kHz, four signed 16-bit channels.
// Bound only the client's software queue, not driver playback latency.
constexpr std::size_t dualSenseHapticFrameBytes = 8;
constexpr std::size_t dualSenseHapticLimitBytes = 48000 * dualSenseHapticFrameBytes * 60 / 1000;
struct HapticQueuePlan {
    std::size_t offset;
    std::size_t bytes;
    bool clear_backlog;
};
constexpr HapticQueuePlan planDualSenseHaptics(std::size_t queued, std::size_t incoming)
{
    const auto whole = wholePcmFrames(incoming, dualSenseHapticFrameBytes);
    const auto bytes = (std::min)(whole, dualSenseHapticLimitBytes);
    return {whole - bytes, bytes, bytes != 0 && queued > dualSenseHapticLimitBytes - bytes};
}
}

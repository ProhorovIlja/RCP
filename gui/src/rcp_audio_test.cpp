// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "rcp/audio_queue.h"

#include <iostream>

using rcp::audioQueueChunk;
using rcp::bufferedAudioMs;
using rcp::wholePcmFrames;

static_assert(bufferedAudioMs(1920, 4, 48000) == 10.0);
static_assert(bufferedAudioMs(9600, 4, 48000) == 50.0);
static_assert(bufferedAudioMs(1920, 0, 48000) < 0.0);
static_assert(bufferedAudioMs(1920, 4, 0) < 0.0);
static_assert(audioQueueChunk(19200, 19200, 1920, 2000, 4) == 0);
static_assert(audioQueueChunk(19200, 23000, 1920, 2000, 4) == 0);
static_assert(audioQueueChunk(19200, 19199, 1920, 2000, 4) == 0);
static_assert(audioQueueChunk(19200, 19189, 1920, 2000, 4) == 8);
static_assert(audioQueueChunk(19200, 0, 1920, 124, 4) == 124);
static_assert(audioQueueChunk(19200, 0, 0, 124, 4) == 0);
static_assert(wholePcmFrames(191, 6) == 186);
static_assert(rcp::dualSenseHapticLimitBytes == 23040);
static_assert(!rcp::planDualSenseHaptics(19200, 3840).clear_backlog);
static_assert(rcp::planDualSenseHaptics(19208, 3840).clear_backlog);
static_assert(rcp::planDualSenseHaptics(0, 23425).offset == 384);
static_assert(rcp::planDualSenseHaptics(0, 23425).bytes == 23040);
static_assert(!rcp::planDualSenseHaptics(24000, 7).clear_backlog);

int main()
{
    // Check alignment and that neither the queue budget nor the ring read can
    // overflow, including odd SDL queue snapshots.
    for (const std::size_t frame_bytes : {2, 4, 6, 8}) {
        for (std::size_t queued = 0; queued < 260; ++queued) {
            for (std::size_t pending = 0; pending < 160; pending += 3) {
                for (std::size_t contiguous = 0; contiguous < 120; contiguous += 7) {
                    constexpr std::size_t target = 192;
                    const auto chunk = audioQueueChunk(target, queued, pending, contiguous, frame_bytes);
                    const auto budget = queued < target ? target - queued : 0;
                    if (chunk % frame_bytes || chunk > budget || chunk > pending || chunk > contiguous ||
                        (budget - chunk >= frame_bytes && pending - chunk >= frame_bytes &&
                         contiguous - chunk >= frame_bytes)) {
                        std::cerr << "Invalid whole-frame chunk for queue " << queued << '\n';
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

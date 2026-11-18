// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "rcp/audio_pull.h"

#include <array>
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

static bool testPullBuffer()
{
    rcp::AudioPullBuffer ring;
    ring.reset(2, 2);
    if (ring.capacity() != 16)
        return false;
    const std::array<unsigned char, 20> input{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20};
    std::array<unsigned char, 5> out{};
    if (ring.push(input.data(), 5) != 0 || ring.pending() != 4)
        return false;
    auto read = ring.render(out.data(), out.size());
    if (read.audio_bytes || read.starved || out != decltype(out){} || ring.pending() != 4)
        return false; // No partial start or stale bytes before the preroll.

    ring.push(input.data() + 4, 4);
    read = ring.render(out.data(), out.size());
    if (read.starved || read.audio_bytes != 4 || out != decltype(out){1, 2, 3, 4, 0})
        return false;
    if (ring.push(input.data(), input.size()) != 8 || ring.pending() != ring.capacity())
        return false; // Overflow keeps the newest complete PCM frames.
    for (unsigned char head : {5, 9, 13, 17}) {
        read = ring.render(out.data(), out.size());
        if (read.starved || read.audio_bytes != 4 || out[0] != head ||
            out[1] != head + 1 || out[2] != head + 2 || out[3] != head + 3 || out[4])
            return false;
    }

    read = ring.render(out.data(), out.size());
    if (!read.starved || read.audio_bytes || out != decltype(out){})
        return false;
    ring.push(input.data(), 4);
    read = ring.render(out.data(), out.size());
    if (read.starved || read.audio_bytes || ring.pending() != 4)
        return false; // Restart only after a complete preroll.
    ring.push(input.data() + 4, 6);
    ring.render(out.data(), 4);
    ring.render(out.data(), 4);
    read = ring.render(out.data(), out.size());
    if (!read.starved || read.audio_bytes != 2 || out != decltype(out){9, 10, 0, 0, 0})
        return false; // Preserve the remaining sample, silence only the missing tail.

    ring.reset();
    read = ring.render(out.data(), out.size());
    return !read.starved && !read.audio_bytes && out == decltype(out){} &&
        ring.push(input.data(), input.size()) == 0 && ring.pending() == 0;
}

static bool testPullJitter()
{
    rcp::AudioPullBuffer ring;
    constexpr unsigned int block = rcp::AudioPullBuffer::requestedFrames;
    constexpr unsigned int frame_bytes = 4;
    ring.reset(frame_bytes, block);
    std::array<unsigned char, 480 * frame_bytes> packet;
    packet.fill(0x31);
    std::array<unsigned char, block * frame_bytes> played{};
    constexpr int jitter[] = {0, 700, -1500, 2100, 0, -2000, 1200};
    unsigned int sent = 0;
    unsigned int started = 0;
    for (unsigned int i = 0; i < 250; ++i) {
        // Host sends 480 frames every 10 ms. Device asks for 512 at 48 kHz.
        const int now = static_cast<int>((1000000ull * i * block) / 48000);
        while (static_cast<int>(sent) * 10000 + jitter[sent % std::size(jitter)] <= now) {
            if (ring.push(packet.data(), packet.size()))
                return false;
            ++sent;
        }
        const auto result = ring.render(played.data(), played.size());
        if (result.starved || (result.audio_bytes && played.front() != 0x31) ||
            (started && result.audio_bytes != played.size()))
            return false;
        if (!started && result.audio_bytes)
            started = i + 1;
    }
    return started >= 2 && started <= 5;
}

int main()
{
    if (!testPullBuffer() || !testPullJitter()) {
        std::cerr << "Experimental pull PCM buffer failed\n";
        return 1;
    }
    std::cout << "Pull PCM tests passed: preroll, wrap, overflow, silence, and paced jitter\n";
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

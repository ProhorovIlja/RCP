// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#pragma once

#include "audio_queue.h"

#include <cstring>
#include <vector>

namespace rcp {
// Game PCM only. The owner serializes push, render, and reset. Render never
// allocates, and fills the entire device block with PCM or signed-PCM silence.
class AudioPullBuffer {
public:
    static constexpr unsigned int requestedFrames = 512;

    struct Read {
        std::size_t audio_bytes = 0;
        bool starved = false;
    };

    void reset(std::size_t frame_bytes = 0, unsigned int callback_frames = 0)
    {
        frame_bytes_ = frame_bytes;
        prime_bytes_ = 2 * std::size_t(callback_frames) * frame_bytes_;
        storage_.resize(2 * prime_bytes_);
        read_pos_ = used_ = 0;
        primed_ = false;
    }

    std::size_t pending() const { return used_; }
    std::size_t capacity() const { return storage_.size(); }

    std::size_t push(const void *pcm, std::size_t bytes)
    {
        bytes = wholePcmFrames(bytes, frame_bytes_);
        const auto cap = capacity();
        if (!cap || !bytes)
            return 0;

        auto input = static_cast<const unsigned char *>(pcm);
        const auto skip = bytes > cap ? bytes - cap : 0;
        input += skip;
        bytes -= skip;
        const auto discard = bytes > cap - used_ ? bytes - (cap - used_) : 0;
        read_pos_ = (read_pos_ + discard) % cap;
        used_ -= discard;

        const auto write = (read_pos_ + used_) % cap;
        const auto first = (std::min)(bytes, cap - write);
        std::memcpy(storage_.data() + write, input, first);
        if (bytes > first)
            std::memcpy(storage_.data(), input + first, bytes - first);
        used_ += bytes;
        return skip + discard;
    }

    Read render(void *pcm, std::size_t bytes)
    {
        if (!bytes)
            return {};
        const auto cap = capacity();
        const auto requested = wholePcmFrames(bytes, frame_bytes_);
        auto out = static_cast<unsigned char *>(pcm);
        Read result;
        if (cap && requested && (primed_ || used_ >= (std::min)(cap, (std::max)(prime_bytes_, requested)))) {
            primed_ = true;
            result.audio_bytes = (std::min)(used_, requested);
            const auto first = (std::min)(result.audio_bytes, cap - read_pos_);
            if (first)
                std::memcpy(out, storage_.data() + read_pos_, first);
            if (result.audio_bytes > first)
                std::memcpy(out + first, storage_.data(), result.audio_bytes - first);
            read_pos_ = (read_pos_ + result.audio_bytes) % cap;
            used_ -= result.audio_bytes;
            result.starved = result.audio_bytes < requested;
            if (result.starved)
                primed_ = false;
        }
        if (bytes > result.audio_bytes)
            std::memset(out + result.audio_bytes, 0, bytes - result.audio_bytes);
        return result;
    }

private:
    std::vector<unsigned char> storage_;
    std::size_t frame_bytes_ = 0;
    std::size_t prime_bytes_ = 0;
    std::size_t read_pos_ = 0;
    std::size_t used_ = 0;
    bool primed_ = false;
};
}

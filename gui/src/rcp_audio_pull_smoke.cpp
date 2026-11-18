// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "rcp/audio_pull.h"

#include <atomic>
#include <cstdio>
#include <vector>

namespace {
struct Smoke {
    rcp::AudioPullBuffer ring;
    std::atomic<unsigned int> data_blocks{0};
    std::atomic<unsigned int> silent_blocks{0};
    std::atomic<unsigned int> invalid_blocks{0};
};

void SDLCALL callback(void *ctx, Uint8 *pcm, int len)
{
    if (len <= 0)
        return;
    auto &test = *static_cast<Smoke *>(ctx);
    const auto read = test.ring.render(pcm, static_cast<std::size_t>(len));
    for (std::size_t i = 0; i < static_cast<std::size_t>(len); ++i) {
        if (pcm[i] != (i < read.audio_bytes ? 0x35 : 0)) {
            test.invalid_blocks.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    auto &counter = read.audio_bytes ? test.data_blocks : test.silent_blocks;
    counter.fetch_add(1, std::memory_order_relaxed);
}
}

int main()
{
    SDL_SetMainReady();
    if (SDL_setenv("SDL_AUDIODRIVER", "dummy", 1) != 0 || SDL_Init(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "SDL dummy audio init: %s\n", SDL_GetError());
        return 1;
    }

    Smoke smoke;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = rcp::AudioPullBuffer::requestedFrames;
    want.callback = callback;
    want.userdata = &smoke;
    const auto dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev || have.freq != want.freq || have.format != want.format ||
        have.channels != want.channels || !have.samples) {
        std::fprintf(stderr, "SDL dummy audio open: %s\n", SDL_GetError());
        if (dev)
            SDL_CloseAudioDevice(dev);
        SDL_Quit();
        return 1;
    }

    smoke.ring.reset(2 * sizeof(Sint16), have.samples);
    const std::vector<unsigned char> input(smoke.ring.capacity(), 0x35);
    smoke.ring.push(input.data(), input.size());
    SDL_PauseAudioDevice(dev, 0);
    for (int i = 0; i < 250 && !smoke.invalid_blocks.load(std::memory_order_relaxed); ++i) {
        if (smoke.data_blocks.load(std::memory_order_relaxed) && smoke.silent_blocks.load(std::memory_order_relaxed))
            break;
        SDL_Delay(10);
    }

    SDL_CloseAudioDevice(dev);
    SDL_Quit();
    const auto good = smoke.data_blocks.load();
    const auto silent = smoke.silent_blocks.load();
    const auto bad = smoke.invalid_blocks.load();
    std::printf("SDL pull dummy callback: pcm_blocks=%u silent_blocks=%u invalid_blocks=%u\n", good, silent, bad);
    return good && silent && !bad ? 0 : 1;
}

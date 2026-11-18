# Stream replay test

Runs the real stream window (decoder, libplacebo renderer, audio output,
microphone, echo cancellation) on recorded media instead of a PS5. Catches
crashes and starved decoders before a build ships.

The replay code exists only when CMake is configured with
`-DNAX5_STREAM_REPLAY=ON`. Release builds use `OFF` (see
`scripts/release/build-alpha05-user-pack.sh`), so product binaries never
contain it.

## Run

```bash
# 1. Media (once): H.264/HEVC/HEVC-HDR like a PS5 sends, Opus 48 kHz 10 ms
python3 scripts/tests/stream-replay/make-replay-media.py /c/astro/worktrees/nax5-replay-media

# 2. Replay-enabled client in its own build dir
cmake -S . -B ../worktrees/nax5-client-replay-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIAKI_ENABLE_CLI=OFF -DNAX5_STREAM_REPLAY=ON
cmake --build ../worktrees/nax5-client-replay-build --target chiaki
```

```powershell
# 3. Matrix (about 8 minutes)
$b = 'C:\astro\worktrees\nax5-client-replay-build'
scripts\tests\stream-replay\run-stream-replay.ps1 -ChiakiExe "$b\gui\chiaki.exe" `
  -MediaRoot C:\astro\worktrees\nax5-replay-media `
  -DllDir "$b\third-party\cpp-steam-tools","C:\msys64\mingw64\bin"
```

For a package made from a replay-enabled build, point `-ChiakiExe` at the bundle's `chiaki.exe` and drop
`-DllDir`. `-Scenario name,...` and `-Repeat N` narrow or repeat runs.

## RCP Windows regression benchmark

GitHub's Windows release check builds a separate replay executable after
assembling the product ZIP and installer, then runs four six-second cases.
It checks queued and pull audio against the same clean trace and the same
impaired trace. Each run carries 30 source scene updates per second in a
60 fps 1080p HEVC stream, 10 ms Opus packets, and 240 scripted input events
that exercise the RCP mouse mapper and merged keyboard/controller state.

The impaired trace uses seed 2406, independent 3% video-access-unit and
2% Opus loss decisions, and holds the last 80 ms of each two-second
interval. It releases only already-due media and input. A/B reports
require identical source slot counts, drops and holds. They include
decoded-frame notifications, successful software swapchain submissions,
the longest frame-ready and GUI-dispatch gaps and pull-buffer counters.

The test uses software decoding, CI-only OpenGL and SDL dummy output.
This is a pre-decoder loss simulation, not a Takion/FEC test, a real cable,
a USB-to-screen measurement or a proxy for NAX5/GTA VI compatibility.
A software swapchain submission does not prove a frame reached a
physical display. Input is injected inside the app and does not test
Windows Raw Input acquisition or PS5 controller reception. Pull and
queued audio counters describe different stages, so an unreported
queued underrun count does not mean zero underruns.

## What a scenario checks

- the process exits cleanly (no crash code, no hang, no crash dump);
- decoded frames >= 95% of accepted access units (50% with loss or corruption);
- the audio device opens; with the microphone on, mic frames flow unless the
  machine has no capture device (reported as `no-device`);
- a decoder that is unavailable on the machine is reported as a fallback, not
  as coverage.

Runs use operator mode (`NAX5-Operator` profile) and a dead API URL, so no
data reaches production. Per-run logs, results and crash dumps are copied to
`-OutDir`.

## Environment the client reads

`NAX5_REPLAY_DIR` (media set), `NAX5_REPLAY_SECONDS`, `NAX5_REPLAY_DECODER`
(`auto`, `none`, `d3d11va`, `vulkan`, ...), `NAX5_REPLAY_MIC`, `NAX5_REPLAY_ECHO`,
`NAX5_REPLAY_LOSS_PCT`, `NAX5_REPLAY_CORRUPT_PCT`, `NAX5_REPLAY_BURST_EVERY_S`,
`NAX5_REPLAY_STALL_MS`, `NAX5_REPLAY_AUDIO_HEADER_ONLY`, `NAX5_REPLAY_RESULT`.

## Found so far

- 2026-09-27: microphone with echo cancellation corrupted the heap on every
  played audio frame (sdl2-compat `SDL_AudioCVT` packing mismatch made the echo
  buffer 0 bytes). Scenario `mic-on-echo`: 6/6 crashes before the fix, 0 after.

# Streaming code references for RCP

RCP is the Windows client for a PS5 Remote Play session brokered by NAX5.
Its client-side work ends at decoding, audio playback, presenting and returning
controller input. The actual source encoder and most network behavior remain
on the PS5 or in existing service infrastructure.

| Project and code | What transfers to RCP | What does not |
| --- | --- | --- |
| [Moonlight Qt pacer](https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/video/ffmpeg-renderers/pacer/pacer.cpp) and [DXGI VSync source](https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/video/ffmpeg-renderers/pacer/dxvsyncsource.cpp) | Keep render work separate from decode, wake on new decoded frames, bound old decoded work and track queue escapes. RCP expedites frame delivery only in its opt-in mode and preserves the existing refresh/recovery watchdog. | Moonlight's exact queue/frame drop rules before measuring RCP's timing and AVFrame lifetimes. |
| [Moonlight Qt D3D11VA](https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/video/ffmpeg-renderers/d3d11va.cpp), [mpv D3D11 settings](https://github.com/mpv-player/mpv/blob/master/DOCS/man/options.rst), [chiaki-rust GPU sink](https://github.com/murilex07/chiaki-rust/blob/main/crates/chiaki-render/src/gpu_sink.rs) | The Windows prototype uses the same-device GPU copy used by established renderers. Measure it against Vulkan with the same clip, and inspect frame age at present. | Driver-dependent direct sampling from decoder textures as a default, or claiming that Rust itself causes the improvement. |
| [Sunshine video encoder](https://github.com/SunshineStream/Sunshine/blob/master/src/video.cpp), [audio capture and Opus](https://github.com/SunshineStream/Sunshine/blob/master/src/audio.cpp), [network sender](https://github.com/SunshineStream/Sunshine/blob/master/src/stream.cpp) | A reference for a separately controlled host and a lab signal generator. Compare where that host puts capture, encoding, packet pacing and repair. | AV1 or NVENC settings on a rented PS5. Installing a Moonlight/Sunshine relay on the receiving PC does not improve the original PS5 stream. |
| [Moonlight SDL audio](https://github.com/moonlight-stream/moonlight-qt/blob/master/app/streaming/audio/renderers/sdlaud.cpp) and [Moonlight RTP/FEC](https://github.com/moonlight-stream/moonlight-common-c/blob/master/src/RtpVideoQueue.c) | Track time in queues and test loss recovery with deterministic packet drops. Keep local backlog, network loss and console behavior separate in reports. | Hard-coding Moonlight's buffering or FEC constants into a PS5 Remote Play protocol. |
| [chiaki-ng haptics fix](https://github.com/chiaki-ng/chiaki-ng/commit/31a0660160078f26dd190a8240971fff7b8e5c8c), [feedback-history fix](https://github.com/chiaki-ng/chiaki-ng/commit/bc397a728bb430f28666129ef006f7a8ac0e6081), [mouse button and wheel PR](https://github.com/chiaki-ng/chiaki-ng/pull/863) | Apply the bounded haptics-queue idea first. Test feedback transitions and the bindings separately on the real transport. | Assuming controller remapping is native mouse input or treating a frequency increase as evidence of responsiveness. |
| [FFmpeg D3D12 HEVC](https://github.com/FFmpeg/FFmpeg/blob/n7.1/libavcodec/d3d12va_hevc.c) and [DirectStorage samples](https://github.com/microsoft/DirectStorage) | D3D12 decode is a candidate if frames can stay on the GPU through presentation and the driver supports the required hardware path. Compare with the already implemented D3D11 lab and the Vulkan baseline. | Using DirectStorage to accelerate the live PS5 bitstream or the game assets loaded by the remote PS5. |

For the first comparison, use identical media, codec, hardware settings, bitrate
and display refresh rate. Include a clip with 30 unique frames carried at 60 fps
and a burst-loss case. Collect frame age, time between displayed updates,
CPU/GPU load, render drops and audio queue resets. Keep recorded-video timing
separate from controller-to-screen timing on the actual PS5. Check alt-tab,
minimize/resume, controller reconnect and pause recovery before enabling a new
mode by default.

## Controller feedback replay

`rcp-feedback-replay` runs the real feedback sender and payload formatter
with a fake clock and a captured send boundary. Windows packaging runs it
automatically. The assertions cover a dropped packet recovered by later
button history, stale and duplicate delivery in the reference receiver, and
16-bit sequence rollover. The receiver is a test model, not PS5 firmware.
It does not cover Takion headers, encryption, the real UDP path or OS
thread scheduling.

The separate idle diagnostic drops the final Cross release and advances
the sender by one second without additional input. The current sender
does not resend that history when idle, even though it still sends
periodic analog/motion-state packets. The diagnostic reports the outcome
without requiring the current limitation to stay in place. This baseline
lets a future protocol backport demonstrate that it fixes the failure
mode before testing on a console.

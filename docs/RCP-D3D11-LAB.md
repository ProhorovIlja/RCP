# RCP D3D11 lab (Windows x64)

This separate executable plays a bundled 2-second synthetic 1080p60 HEVC clip
locally. It does not connect to NAX5, read its settings or use a console
session. The main launcher remains unchanged. Run from PowerShell in the
extracted NAX5 folder so the lab finds the bundled media and DLLs.

    .\rcp-d3d11-lab.exe

A short test pattern opens in a window and the console prints a frame count,
NV12/P010 and CPU-side renderer/submit timing. Look for:

    path=D3D11VA->same-device-GPU-copy->DXGI cpu_readbacks=0

This means FFmpeg provided D3D11VA decoder surfaces and we submitted them
without a video-frame readback to system RAM. There is still a copy **on the
GPU** from the decoder array into a reusable, shader-readable resource.
libplacebo samples its luma/chroma views on the same D3D11 device and
presents to a DXGI swapchain. The lab rejects software fallback. It exits
nonzero if the device, decode format, texture or swapchain is unsupported.
An optional argument selects a different local H.264/HEVC Matroska clip
(maximum 3840x2160 and 180 presented frames). This is an SDR lab, not a test
of HDR output even if the decoder reports a P010 surface.

The first frame may include shader setup. The p50/p95 samples describe the
CPU-side copy/render/present call, including possible swapchain pacing. They
are not GPU execution time, monitor display time, network delay, controller
response, or a result comparable to a live Vulkan session.

For CI and troubleshooting:

    .\rcp-d3d11-lab.exe --warp-smoke

This synthetic software WARP test checks array-slice copying, wrapping a
D3D11 shader view, drawing and reading back the expected pixels. That
validation readback is only in the smoke test. The WARP test does **not**
validate a hardware video decoder, physical display or the live launcher.

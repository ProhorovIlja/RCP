// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Standalone offline probe. It is not linked into the NAX5 launcher.
#include <windows.h>
#include <d3d11.h>

#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/d3d11.h>
#include <libplacebo/utils/libav.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

template <class T>
struct Com {
    T *ptr = nullptr;
    Com() = default;
    Com(const Com &) = delete;
    Com &operator=(const Com &) = delete;
    ~Com() { reset(); }
    void reset()
    {
        if (ptr)
            ptr->Release();
        ptr = nullptr;
    }
    T **put() { reset(); return &ptr; }
    T *operator->() const { return ptr; }
};

struct Decode {
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVBufferRef *hardware = nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    ~Decode()
    {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        av_buffer_unref(&hardware);
        avformat_close_input(&format);
    }
};

struct Render {
    pl_log log = nullptr;
    pl_d3d11 device = nullptr;
    pl_renderer renderer = nullptr;
    pl_swapchain swap = nullptr;
    HWND window = nullptr;
    Com<ID3D11Texture2D> sample_texture; // GPU-only, not a CPU staging resource
    std::array<pl_tex, 2> planes{};
    D3D11_TEXTURE2D_DESC desc{};

    ~Render()
    {
        if (device) {
            for (auto &plane : planes)
                pl_tex_destroy(device->gpu, &plane);
        }
        pl_renderer_destroy(&renderer);
        pl_swapchain_destroy(&swap);
        pl_d3d11_destroy(&device);
        if (window)
            DestroyWindow(window);
        pl_log_destroy(&log);
    }

    bool init(ID3D11Device *existing, bool warp)
    {
        pl_log_params log_params{};
        log_params.log_cb = pl_log_simple;
        log_params.log_level = PL_LOG_WARN;
        log = pl_log_create(PL_API_VER, &log_params);
        pl_d3d11_params params{};
        params.device = existing;
        params.force_software = warp;
        device = pl_d3d11_create(log, &params);
        if (!device || device->software != warp)
            return false;
        renderer = pl_renderer_create(log, device->gpu);
        return renderer != nullptr;
    }

    bool texture(unsigned width, unsigned height, DXGI_FORMAT format, unsigned mips,
                 D3D11_SUBRESOURCE_DATA *initial, ID3D11Texture2D **out, unsigned slices = 1)
    {
        D3D11_TEXTURE2D_DESC spec{};
        spec.Width = width;
        spec.Height = height;
        spec.MipLevels = mips;
        spec.ArraySize = slices;
        spec.Format = format;
        spec.SampleDesc.Count = 1;
        spec.Usage = D3D11_USAGE_DEFAULT;
        spec.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(device->device->CreateTexture2D(&spec, initial, out));
    }

    bool preparePlanes(const D3D11_TEXTURE2D_DESC &source)
    {
        if (sample_texture.ptr && desc.Width == source.Width && desc.Height == source.Height &&
            desc.Format == source.Format && planes[0] && planes[1])
            return true;
        for (auto &plane : planes)
            pl_tex_destroy(device->gpu, &plane);
        const bool p010 = source.Format == DXGI_FORMAT_P010;
        if (!p010 && source.Format != DXGI_FORMAT_NV12)
            return false;
        if (source.Width % 2 || source.Height % 2 ||
            !texture(source.Width, source.Height, source.Format, 1, nullptr, sample_texture.put()))
            return false;
        desc = source;

        pl_d3d11_wrap_params p{};
        p.tex = sample_texture.ptr;
        p.fmt = p010 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        p.w = static_cast<int>(source.Width);
        p.h = static_cast<int>(source.Height);
        planes[0] = pl_d3d11_wrap(device->gpu, &p);
        p.fmt = p010 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        p.w /= 2;
        p.h /= 2;
        planes[1] = pl_d3d11_wrap(device->gpu, &p);
        return planes[0] && planes[1];
    }

    bool present(const AVFrame *frame, AVD3D11VADeviceContext *decode)
    {
        if (frame->format != AV_PIX_FMT_D3D11 || !frame->hw_frames_ctx || !frame->data[0])
            return false;
        auto *source = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
        const auto slice = reinterpret_cast<std::uintptr_t>(frame->data[1]);
        D3D11_TEXTURE2D_DESC spec{};
        source->GetDesc(&spec);
        if (slice >= spec.ArraySize || spec.MipLevels != 1 || spec.SampleDesc.Count != 1 ||
            !preparePlanes(spec))
            return false;
        Com<ID3D11Device> owner;
        source->GetDevice(owner.put());
        if (owner.ptr != device->device)
            return false;

        // Decoder arrays are not necessarily shader-readable. Copy one slice
        // to a reusable texture on the same device. No Map or CPU transfer.
        decode->lock(decode->lock_ctx);
        decode->device_context->CopySubresourceRegion(sample_texture.ptr, 0, 0, 0, 0, source,
            D3D11CalcSubresource(0, static_cast<UINT>(slice), spec.MipLevels), nullptr);
        decode->unlock(decode->lock_ctx);

        struct pl_frame image{};
        pl_frame_from_avframe(&image, frame);
        image.num_planes = 2;
        image.planes[0].texture = planes[0];
        image.planes[0].components = 1;
        image.planes[0].component_mapping[0] = 0;
        image.planes[1].texture = planes[1];
        image.planes[1].components = 2;
        image.planes[1].component_mapping[0] = 1;
        image.planes[1].component_mapping[1] = 2;
        image.repr.bits.sample_depth = spec.Format == DXGI_FORMAT_P010 ? 16 : 8;
        image.repr.bits.color_depth = spec.Format == DXGI_FORMAT_P010 ? 10 : 8;
        image.repr.bits.bit_shift = spec.Format == DXGI_FORMAT_P010 ? 6 : 0;
        pl_frame_set_chroma_location(&image, PL_CHROMA_LEFT);

        struct pl_swapchain_frame back{};
        if (!pl_swapchain_start_frame(swap, &back))
            return false;
        struct pl_frame target{};
        pl_frame_from_swapchain(&target, &back);
        const bool rendered = pl_render_image(renderer, &image, &target, &pl_render_fast_params);
        const bool submitted = pl_swapchain_submit_frame(swap);
        if (rendered && submitted)
            pl_swapchain_swap_buffers(swap);
        return rendered && submitted;
    }
};

LRESULT CALLBACK labWindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool openWindow(Render &r)
{
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = labWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"RcpD3d11Lab";
    if (!RegisterClassW(&wc))
        return false;
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rect{0, 0, 960, 540};
    if (!AdjustWindowRect(&rect, style, false))
        return false;
    r.window = CreateWindowExW(0, wc.lpszClassName, L"RCP D3D11 lab (offline)", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, instance, nullptr);
    if (!r.window)
        return false;
    pl_d3d11_swapchain_params params{};
    params.window = r.window;
    params.disable_10bit_sdr = true;
    r.swap = pl_d3d11_create_swapchain(r.device, &params);
    if (!r.swap)
        return false;
    ShowWindow(r.window, SW_SHOW);
    return true;
}

bool keepWindow()
{
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

AVPixelFormat selectD3D11(AVCodecContext *, const AVPixelFormat *formats)
{
    for (; *formats != AV_PIX_FMT_NONE; ++formats) {
        if (*formats == AV_PIX_FMT_D3D11)
            return *formats;
    }
    return AV_PIX_FMT_NONE; // Software fallback would invalidate this test.
}

int fail(const char *stage)
{
    std::fprintf(stderr, "RCP D3D11 lab failed: %s\n", stage);
    return 1;
}

int play(const char *path)
{
    Decode d;
    if (!d.packet || !d.frame)
        return fail("FFmpeg allocation");
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    const auto *mkv = av_find_input_format("matroska");
    const int opened = mkv ? avformat_open_input(&d.format, path, mkv, &options) : AVERROR_INVALIDDATA;
    av_dict_free(&options);
    if (opened < 0)
        return fail("open local Matroska clip");

    // The bundled clip declares its codec in the container. Do not ask FFmpeg
    // to software-decode sample packets in avformat_find_stream_info().
    int track = -1;
    for (unsigned i = 0; i < d.format->nb_streams; ++i) {
        const auto *p = d.format->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && (p->codec_id == AV_CODEC_ID_H264 || p->codec_id == AV_CODEC_ID_HEVC)) {
            track = static_cast<int>(i);
            break;
        }
    }
    if (track < 0)
        return fail("clip has no H.264 or HEVC track");
    const auto *params = d.format->streams[track]->codecpar;
    if (params->width <= 0 || params->height <= 0 || params->width > 3840 || params->height > 2160)
        return fail("clip resolution outside the lab's supported range");
    const auto *codec = avcodec_find_decoder(params->codec_id);
    if (!codec)
        return fail("FFmpeg decoder unavailable");
    d.codec = avcodec_alloc_context3(codec);
    if (!d.codec || avcodec_parameters_to_context(d.codec, params) < 0)
        return fail("initialize codec parameters");
    if (av_hwdevice_ctx_create(&d.hardware, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
        return fail("create D3D11VA hardware device");
    const auto *hw = reinterpret_cast<const AVHWDeviceContext *>(d.hardware->data);
    auto *device = static_cast<AVD3D11VADeviceContext *>(hw->hwctx);
    d.codec->hw_device_ctx = av_buffer_ref(d.hardware);
    d.codec->get_format = selectD3D11;
    d.codec->thread_count = 1;
    if (!d.codec->hw_device_ctx || avcodec_open2(d.codec, codec, nullptr) < 0)
        return fail("open hardware decoder");

    Render r;
    if (!r.init(device->device, false) || !openWindow(r))
        return fail("same-device libplacebo / DXGI setup");
    std::vector<double> submit;
    submit.reserve(180);
    bool stopped = false;
    auto deadline = Clock::now();
    constexpr auto tick = std::chrono::microseconds(16667);
    auto drain = [&]() {
        int status;
        while ((status = avcodec_receive_frame(d.codec, d.frame)) == 0) {
            if (!keepWindow() || submit.size() >= 180) {
                stopped = true;
                av_frame_unref(d.frame);
                return true;
            }
            std::this_thread::sleep_until(deadline);
            const auto start = Clock::now();
            const bool ok = r.present(d.frame, device);
            const auto end = Clock::now();
            av_frame_unref(d.frame);
            if (!ok)
                return false;
            submit.push_back(std::chrono::duration<double, std::milli>(end - start).count());
            deadline = (std::max)(deadline + tick, start);
        }
        return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
    };
    int status = 0;
    while (!stopped && (status = av_read_frame(d.format, d.packet)) == 0) {
        bool ok = true;
        if (d.packet->stream_index == track)
            ok = avcodec_send_packet(d.codec, d.packet) >= 0 && drain();
        av_packet_unref(d.packet);
        if (!ok)
            return fail("D3D11VA decode, same-device copy, or DXGI presentation");
    }
    if (!stopped && (status != AVERROR_EOF || avcodec_send_packet(d.codec, nullptr) < 0 || !drain()))
        return fail("clip read or decoder flush");
    if (submit.empty())
        return fail("no hardware frames presented");
    const double first = submit.front();
    std::sort(submit.begin(), submit.end());
    const size_t p95 = (submit.size() * 95 + 99) / 100 - 1;
    std::printf("frames=%zu format=%s path=D3D11VA->same-device-GPU-copy->DXGI cpu_readbacks=0\n",
        submit.size(), r.desc.Format == DXGI_FORMAT_P010 ? "P010" : "NV12");
    std::printf("cpu_copy_render_present_call_ms: first=%.2f p50=%.2f p95=%.2f\n",
        first, submit[(submit.size() - 1) / 2], submit[p95]);
    std::puts("Offline fixed-60Hz feed. These timings include submit pacing, not display or input-to-photon delay.");
    return 0;
}

int warpSmoke()
{
    Render r;
    if (!r.init(nullptr, true))
        return fail("D3D11 WARP init");
    constexpr unsigned dim = 16;
    std::array<uint8_t, dim * dim * 4> first{}, second{}, result{};
    // Distinct slices. A successful white output proves nonzero array index,
    // view wrapping, drawing and sample_texture verification, not a hardware decode.
    for (size_t i = 0; i < second.size(); i += 4) {
        first[i + 3] = 255;
        second[i] = second[i + 1] = second[i + 2] = second[i + 3] = 255;
    }
    D3D11_SUBRESOURCE_DATA data[]{{first.data(), dim * 4, 0}, {second.data(), dim * 4, 0}};
    Com<ID3D11Texture2D> source, destination;
    if (!r.texture(dim, dim, DXGI_FORMAT_R8G8B8A8_UNORM, 1, data, source.put(), 2) ||
        !r.texture(dim, dim, DXGI_FORMAT_R8G8B8A8_UNORM, 1, nullptr, destination.put()))
        return fail("WARP textures");
    Com<ID3D11DeviceContext> context;
    r.device->device->GetImmediateContext(context.put());
    context->CopySubresourceRegion(destination.ptr, 0, 0, 0, 0, source.ptr, 1, nullptr);
    pl_d3d11_wrap_params wrap{};
    wrap.tex = destination.ptr;
    pl_tex sampled = pl_d3d11_wrap(r.device->gpu, &wrap);
    struct pl_tex_params spec{};
    spec.w = spec.h = dim;
    spec.format = pl_find_named_fmt(r.device->gpu, "rgba8");
    spec.renderable = true;
    spec.host_readable = true;
    pl_tex target_tex = spec.format ? pl_tex_create(r.device->gpu, &spec) : nullptr;
    auto rgba = [](pl_tex tex) {
        struct pl_frame image{};
        image.num_planes = 1;
        image.planes[0].texture = tex;
        image.planes[0].components = 4;
        for (int i = 0; i < 4; ++i)
            image.planes[0].component_mapping[i] = i;
        image.crop = {0, 0, dim, dim};
        image.repr = pl_color_repr_rgb;
        image.repr.alpha = PL_ALPHA_INDEPENDENT;
        image.color = pl_color_space_srgb;
        return image;
    };
    struct pl_tex_transfer_params readback{};
    readback.tex = target_tex;
    readback.ptr = result.data();
    const auto image = rgba(sampled), target = rgba(target_tex);
    const bool ok = sampled && target_tex &&
        pl_render_image(r.renderer, &image, &target, &pl_render_fast_params) &&
        pl_tex_download(r.device->gpu, &readback) &&
        std::all_of(result.begin(), result.end(), [](uint8_t x) { return x >= 250; });
    pl_tex_destroy(r.device->gpu, &target_tex);
    pl_tex_destroy(r.device->gpu, &sampled);
    if (!ok)
        return fail("WARP copy / wrap / draw / pixel validation");
    std::puts("WARP smoke passed: array-slice copy, sampled draw, expected pixels (software; no hardware decode or DXGI claim).");
    return 0;
}
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--warp-smoke") == 0)
        return warpSmoke();
    if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
        std::puts("rcp-d3d11-lab.exe [local H.264/HEVC Matroska clip] | --warp-smoke");
        return 0;
    }
    if (argc > 2)
        return fail("expected one local file or a lab option");
    return play(argc == 2 ? argv[1] : "rcp-d3d11-demo.mkv");
}

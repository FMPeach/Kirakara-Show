#include "kirakara/show/config.hpp"
#include "kirakara/show/lrc_parser.hpp"
#include "kirakara/show/project_parser.hpp"
#include "kirakara/show/song_title.hpp"
#include "kirakara/show/win32/d3d11_texture_surface.hpp"
#include "kirakara/show/win32/unified_stage_renderer.hpp"
#include "../apps/show_host/media/mf_d3d11_video_decoder.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace kirakara::show;
using namespace kirakara::show::win32;

namespace {

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::vector<std::uint8_t> read_bgra(void* native_texture,
        std::uint32_t expected_width, std::uint32_t expected_height) {
    auto* texture = static_cast<ID3D11Texture2D*>(native_texture);
    if (!texture) return {};
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    if (description.Width != expected_width
            || description.Height != expected_height) return {};
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    texture->GetDevice(&device);
    if (!device) return {};
    device->GetImmediateContext(&context);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ID3D11Texture2D* staging{};
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) {
        release(context);
        release(device);
        return {};
    }
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        release(staging);
        release(context);
        release(device);
        return {};
    }
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(expected_width) * expected_height * 4U);
    for (std::uint32_t y = 0; y < expected_height; ++y) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto* destination = pixels.data()
            + static_cast<std::size_t>(y) * expected_width * 4U;
        std::copy_n(source, static_cast<std::size_t>(expected_width) * 4U,
            destination);
    }
    context->Unmap(staging, 0);
    release(staging);
    release(context);
    release(device);
    return pixels;
}

bool is_black(const std::uint8_t* pixel) {
    return pixel[0] < 4 && pixel[1] < 4 && pixel[2] < 4 && pixel[3] > 250;
}

bool is_green(const std::uint8_t* pixel) {
    return pixel[0] < 4 && pixel[1] > 250 && pixel[2] < 4 && pixel[3] > 250;
}

bool is_red(const std::uint8_t* pixel) {
    return pixel[0] < 4 && pixel[1] < 4 && pixel[2] > 250 && pixel[3] > 250;
}

bool is_blue(const std::uint8_t* pixel) {
    return pixel[0] > 250 && pixel[1] < 4 && pixel[2] < 4 && pixel[3] > 250;
}

std::uint64_t sparse_hash(const std::vector<std::uint8_t>& pixels) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto stride = std::max<std::size_t>(4, pixels.size() / 8192U);
    for (std::size_t index = 0; index < pixels.size(); index += stride) {
        hash ^= pixels[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

AppConfig test_config() {
    auto config = default_app_config();
    config.font_family = L"Microsoft YaHei";
    config.font_families = {L"Microsoft YaHei", L"sans-serif"};
    config.song_title.enabled = true;
    config.song_title.duration = 5.0;
    config.song_title.text_fade = false;
    config.song_title.blocks.clear();
    SongTitleBlock title;
    title.lines = {"UNIFIED STAGE"};
    title.style.font_family = L"Arial";
    title.style.font_families = {L"Arial"};
    title.style.font_size = 64.0F;
    title.style.y = 105.0F;
    config.song_title.blocks.push_back(std::move(title));
    return config;
}

std::shared_ptr<const DecodedD3D11VideoFrame> wait_for_video_frame(
        const MfD3D11VideoDecoder& decoder, const std::wstring& source,
        std::uint64_t revision, double position) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto frame = decoder.texture_for(source, revision, position)) {
            if (std::abs(frame->timestamp_seconds() - position) <= 0.0501) {
                return frame;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

ProjectParseResult read_project(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    expect(static_cast<bool>(stream), "open real KRL fixture");
    const std::string text{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    return parse_project_text(text);
}

void render_real_frame(UnifiedStageRenderer& renderer,
        StageFrameMailbox& mailbox,
        const DecodedD3D11VideoFrame& video,
        const ProjectParseResult& project,
        std::uint64_t generation,
        std::uint64_t frame_id,
        double position) {
    UnifiedStageRenderRequest request{
        .timing = StageFrameTiming{
            generation,
            frame_id,
            static_cast<std::int64_t>(position * 10'000'000.0),
            166'667,
        },
        .project_time = position,
        .video_texture = video.texture(),
        .video_width = video.width(),
        .video_height = video.height(),
        .video_identity = UnifiedStageVideoFrameIdentity{
            .valid = true,
            .generation = video.generation(),
            .frame_id = video.frame_id(),
            .width = video.width(),
            .height = video.height(),
        },
        .lyrics = &project.document,
        .config = &project.config,
    };
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "compose decoded video with real KRL");
    auto stage_frame = mailbox.take_latest();
    expect(stage_frame && stage_frame->timing.generation == generation,
        "real Stage frame published with current generation");
}

void validate_video_base_cache() {
    constexpr std::uint32_t output_width = 640;
    constexpr std::uint32_t output_height = 360;
    constexpr std::uint32_t source_width = 320;
    constexpr std::uint32_t source_height = 240;
    UnifiedStageRenderer renderer;
    expect(renderer.configure(StageOutputProfile{
        StageOutputRole::controller_preview,
        output_width,
        output_height,
        60,
        1,
        StagePixelFormat::bgra8,
    }), "configure cached-video Stage");
    auto mailbox = renderer.subscribe();

    D3D11TextureSurface source;
    expect(source.resize_on_device(renderer.native_d3d_device(),
        source_width, source_height), "create cached-video source");
    const auto fill_source = [&source](Color color) {
        source.begin_draw();
        source.clear(color);
        expect(source.end_draw(), "fill cached-video source");
        source.flush_d2d();
    };
    fill_source(Color{0.0F, 1.0F, 0.0F, 1.0F});

    const auto parsed = parse_lrc(
        "[00:01.00]K[00:01.40]i[00:01.80]r[00:02.20]a[00:03.50]");
    auto config = test_config();
    UnifiedStageRenderTimings diagnostics{};
    UnifiedStageRenderRequest request{
        .timing = StageFrameTiming{10, 0, 11'000'000, 166'667},
        .project_time = 1.1,
        .video_texture = source.native_texture(),
        .video_width = source_width,
        .video_height = source_height,
        .video_identity = UnifiedStageVideoFrameIdentity{
            .valid = true,
            .generation = 10,
            .frame_id = 7,
            .width = source_width,
            .height = source_height,
        },
        .lyrics = &parsed.document,
        .config = &config,
        .draw_song_title = false,
        .diagnostics = &diagnostics,
    };
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "publish first cached video base");
    expect(diagnostics.video_cache_refreshed
            && !diagnostics.video_cache_reused,
        "first decoded identity refreshes video base");
    expect(diagnostics.d2d_flush_count == 2,
        "cache refresh flushes once before copy and once before publish");
    expect(diagnostics.d3d_flush_count == 0,
        "same-device cached Stage does not force a D3D flush");
    auto frame = mailbox.take_latest();
    expect(static_cast<bool>(frame), "receive first cached video frame");
    auto pixels = read_bgra(
        frame->native_resource, output_width, output_height);
    const auto center_index =
        (static_cast<std::size_t>(output_height / 2U) * output_width
            + output_width / 2U) * 4U;
    expect(is_green(pixels.data() + center_index),
        "first cached video base contains source pixels");
    frame = {};

    // Mutating the decoder texture while retaining its declared identity
    // simulates a pooled texture being reused behind an already cached frame.
    // The output must keep the captured base while dynamic lyrics advance.
    fill_source(Color{1.0F, 0.0F, 0.0F, 1.0F});
    request.timing.frame_id = 1;
    request.timing.pts_100ns = 26'000'000;
    request.project_time = 2.6;
    diagnostics = {};
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "publish repeated decoded identity");
    expect(!diagnostics.video_cache_refreshed
            && diagnostics.video_cache_reused,
        "repeated decoded identity reuses video base");
    expect(diagnostics.d2d_flush_count == 1,
        "cached video reuse only flushes the final Stage target");
    frame = mailbox.take_latest();
    expect(static_cast<bool>(frame), "receive reused video frame");
    auto reused_pixels = read_bgra(
        frame->native_resource, output_width, output_height);
    expect(is_green(reused_pixels.data() + center_index),
        "reused video base ignores pooled source mutation");
    expect(reused_pixels != pixels,
        "dynamic lyric overlay advances over cached video base");
    frame = {};

    request.timing.frame_id = 2;
    request.video_identity.frame_id = 8;
    diagnostics = {};
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "publish next decoded identity");
    expect(diagnostics.video_cache_refreshed,
        "new frame id refreshes video base");
    expect(diagnostics.d2d_flush_count == 2,
        "new decoded identity restores the video-base copy boundary");
    frame = mailbox.take_latest();
    auto refreshed_pixels = read_bgra(
        frame->native_resource, output_width, output_height);
    expect(is_red(refreshed_pixels.data() + center_index),
        "new frame id captures changed source pixels");
    frame = {};

    fill_source(Color{0.0F, 0.0F, 1.0F, 1.0F});
    request.timing = StageFrameTiming{11, 0, 0, 166'667};
    request.video_identity.generation = 11;
    diagnostics = {};
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "publish new decoded generation");
    expect(diagnostics.video_cache_refreshed,
        "generation change invalidates video base");
    frame = mailbox.take_latest();
    auto generation_pixels = read_bgra(
        frame->native_resource, output_width, output_height);
    expect(is_blue(generation_pixels.data() + center_index),
        "new generation captures current source pixels");
    frame = {};

    request.timing.frame_id = 1;
    request.video_texture = nullptr;
    request.video_width = 0;
    request.video_height = 0;
    request.video_identity = {};
    request.lyrics = nullptr;
    request.config = nullptr;
    request.flush_d3d_before_publish = true;
    diagnostics = {};
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "publish idle frame after cached video");
    expect(diagnostics.d2d_flush_count == 1,
        "idle frame has one final D2D submission boundary");
    expect(diagnostics.d3d_flush_count == 1,
        "Flutter-only publication keeps its cross-device D3D flush");
    frame = mailbox.take_latest();
    auto idle_pixels = read_bgra(
        frame->native_resource, output_width, output_height);
    expect(is_black(idle_pixels.data() + center_index),
        "idle frame does not leak cached video base");
}

void validate_real_media(const std::wstring& first_video,
        const std::filesystem::path& first_krl,
        const std::wstring& second_video) {
    UnifiedStageRenderer renderer;
    expect(renderer.configure(StageOutputProfile{}),
        "configure real-media Stage");
    auto mailbox = renderer.subscribe();
    auto project = read_project(first_krl);
    expect(!project.document.lines.empty(), "real KRL contains lyrics");

    MfD3D11VideoDecoder decoder;
    expect(decoder.start(renderer.native_d3d_device()),
        "start MF decoder on Unified Stage device");

    // Cold start and HTTP sources exercise the same progressive SourceReader
    // path used by ShowHost. A frame may be absent while buffering, but once
    // available it is never copied through the CPU before Stage composition.
    decoder.synchronize(first_video, 10, 0.0, MfD3D11PlaybackState::paused);
    auto frame = wait_for_video_frame(decoder, first_video, 10, 0.0);
    expect(frame != nullptr, "cold-start decoded frame");
    expect(std::abs(frame->timestamp_seconds()) <= 0.0501,
        "cold-start video is within 50 ms of presentation time");
    render_real_frame(renderer, mailbox, *frame, project, 10, 0, 0.0);

    std::uint64_t frame_id = 1;
    for (const double position : {0.10, 0.25, 0.50}) {
        decoder.synchronize(first_video, 10, position,
            MfD3D11PlaybackState::playing);
        frame = wait_for_video_frame(decoder, first_video, 10, position);
        expect(frame != nullptr, "playing decoded frame");
        expect(std::abs(frame->timestamp_seconds() - position) <= 0.0501,
            "playing video is within 50 ms of presentation time");
        render_real_frame(renderer, mailbox, *frame, project,
            10, frame_id++, position);
    }

    decoder.synchronize(first_video, 10, 0.50,
        MfD3D11PlaybackState::paused);
    frame = wait_for_video_frame(decoder, first_video, 10, 0.50);
    expect(frame != nullptr, "paused frame retained");
    render_real_frame(renderer, mailbox, *frame, project,
        10, frame_id++, 0.50);

    // Replay is a new generation at media zero; no texture from the previous
    // generation may be reused.
    decoder.synchronize(first_video, 11, 0.0,
        MfD3D11PlaybackState::paused);
    expect(decoder.texture_for(first_video, 11, 0.0) == nullptr,
        "replay rejects old decoded texture");
    frame = wait_for_video_frame(decoder, first_video, 11, 0.0);
    expect(frame != nullptr, "replay decoded first frame");
    render_real_frame(renderer, mailbox, *frame, project, 11, 0, 0.0);

    if (!second_video.empty()) {
        // Manual and natural queue advances share this identity transition.
        // The Stage generation changes without rebuilding the renderer/pool.
        decoder.synchronize(second_video, 12, 0.0,
            MfD3D11PlaybackState::paused);
        expect(decoder.texture_for(second_video, 12, 0.0) == nullptr,
            "song switch rejects previous source texture");
        frame = wait_for_video_frame(decoder, second_video, 12, 0.0);
        expect(frame != nullptr, "next-song decoded first frame");
        render_real_frame(renderer, mailbox, *frame, project, 12, 0, 0.0);
    }

    decoder.synchronize(L"", 13, 0.0, MfD3D11PlaybackState::idle);
    decoder.stop();
}

void validate_profile(std::uint32_t width, std::uint32_t height) {
    UnifiedStageRenderer renderer;
    const StageOutputProfile profile{
        StageOutputRole::controller_preview,
        width,
        height,
        60,
        1,
        StagePixelFormat::bgra8,
    };
    expect(renderer.configure(profile), "configure variable-size Stage");
    expect(renderer.native_d3d_device() != nullptr, "shared D3D11 device");

    D3D11TextureSurface source;
    expect(source.resize_on_device(renderer.native_d3d_device(), 320, 240),
        "source texture on Stage device");
    source.begin_draw();
    source.clear(Color{0.0F, 1.0F, 0.0F, 1.0F});
    expect(source.end_draw(), "green source frame");
    source.flush_d2d();

    const auto parsed = parse_lrc(
        "[00:01.00]K[00:01.40]i[00:01.80]r[00:02.20]a[00:03.50]");
    expect(!parsed.document.lines.empty(), "lyric fixture parse");
    auto config = test_config();
    StageOverlayState overlays;
    overlays.revision = 1;
    overlays.qr.visible = true;
    overlays.qr.payload = L"http://192.0.2.1:7391";
    overlays.announcement.visible = true;
    overlays.announcement.text = L"Kira Karaoke";
    auto mailbox = renderer.subscribe();
    UnifiedStageRenderRequest request{
        .timing = StageFrameTiming{1, 1, 25'000'000, 166'667},
        .project_time = 2.5,
        .video_texture = source.native_texture(),
        .video_width = 320,
        .video_height = 240,
        .lyrics = &parsed.document,
        .config = &config,
        .overlays = &overlays,
    };
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "publish real Stage frame");
    auto frame = mailbox.take_latest();
    expect(static_cast<bool>(frame), "Stage Sink receives frame");
    expect(frame->profile.width == width && frame->profile.height == height,
        "published profile dimensions");
    expect(frame->timing.generation == 1 && frame->timing.frame_id == 1,
        "published timing metadata");
    expect(frame->shared_handle != 0,
        "Unified Stage frame exposes a DXGI shared handle");

    auto* producer_device = static_cast<ID3D11Device*>(
        renderer.native_d3d_device());
    IDXGIDevice* producer_dxgi_device{};
    IDXGIAdapter* producer_adapter{};
    expect(SUCCEEDED(producer_device->QueryInterface(
            IID_PPV_ARGS(&producer_dxgi_device))),
        "query producer DXGI device");
    expect(SUCCEEDED(producer_dxgi_device->GetAdapter(&producer_adapter)),
        "query producer DXGI adapter");
    ID3D11Device* consumer_device{};
    ID3D11DeviceContext* consumer_context{};
    D3D_FEATURE_LEVEL selected{};
    constexpr D3D_FEATURE_LEVEL levels[]{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    expect(SUCCEEDED(D3D11CreateDevice(
            producer_adapter,
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            &consumer_device,
            &selected,
            &consumer_context)),
        "create same-adapter consumer device");
    ID3D11Resource* shared_resource{};
    const auto open_shared_result = consumer_device->OpenSharedResource(
            reinterpret_cast<HANDLE>(frame->shared_handle),
            IID_PPV_ARGS(&shared_resource));
    if (FAILED(open_shared_result)) {
        std::cerr << "OpenSharedResource HRESULT=0x" << std::hex
                  << static_cast<unsigned long>(open_shared_result) << '\n';
    }
    expect(SUCCEEDED(open_shared_result),
        "consumer device opens Unified Stage shared handle");
    release(shared_resource);
    release(consumer_context);
    release(consumer_device);
    release(producer_adapter);
    release(producer_dxgi_device);

    const auto pixels = read_bgra(frame->native_resource, width, height);
    expect(pixels.size() == static_cast<std::size_t>(width) * height * 4U,
        "GPU Stage diagnostic readback");
    const auto* top_left = pixels.data();
    const auto center_index =
        (static_cast<std::size_t>(height / 2U) * width + width / 2U) * 4U;
    expect(is_black(top_left), "4:3 video has opaque black pillarbox");
    expect(is_green(pixels.data() + center_index),
        "video is letterboxed into final Stage target");

    std::size_t top_overlay_pixels{};
    std::size_t bottom_overlay_pixels{};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = width / 8U; x < width * 7U / 8U; ++x) {
            const auto* pixel = pixels.data()
                + (static_cast<std::size_t>(y) * width + x) * 4U;
            if (is_black(pixel) || is_green(pixel)) continue;
            if (y < height * 2U / 5U) ++top_overlay_pixels;
            if (y > height / 2U) ++bottom_overlay_pixels;
        }
    }
    expect(top_overlay_pixels > width / 2U,
        "title is composed in shared design viewport");
    expect(bottom_overlay_pixels > width / 8U,
        "KRL lyrics are composed in shared design viewport");

    const auto first_hash = sparse_hash(pixels);
    request.timing.frame_id = 2;
    request.overlays = nullptr;
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "paused presentation publishes stable replacement frame");
    auto paused = mailbox.take_latest();
    expect(static_cast<bool>(paused), "paused frame received");
    const auto paused_pixels = read_bgra(paused->native_resource, width, height);
    expect(sparse_hash(paused_pixels) == first_hash,
        "Phase 6 overlay state does not draw temporary pixels");

    request.timing = StageFrameTiming{2, 0, 0, 166'667};
    request.project_time = 0.0;
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "new song generation starts at frame zero");
    auto next_generation = mailbox.take_latest();
    expect(next_generation && next_generation->timing.generation == 2,
        "Sink accepts new generation");

    frame = {};
    paused = {};
    next_generation = {};
    request.timing = StageFrameTiming{1, 99, 30'000'000, 166'667};
    expect(renderer.render(request) == UnifiedStageRenderResult::published,
        "producer can finish stale work without blocking");
    expect(!mailbox.take_latest(), "Sink rejects stale generation");
    expect(renderer.rendered_frames() == 4, "render statistics");
    expect(renderer.dropped_frames() == 0, "no pool drops");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
        "COM initialization");
    expect(SUCCEEDED(MFStartup(MF_VERSION)), "Media Foundation startup");
    validate_video_base_cache();
    validate_profile(1920, 1080);
    validate_profile(2560, 1440);
    validate_profile(3840, 2160);
    if (argc >= 3) {
        validate_real_media(argv[1], argv[2], argc >= 4 ? argv[3] : L"");
    }
    MFShutdown();
    CoUninitialize();
    std::cout << "Unified Stage variable-profile tests passed.\n";
    return 0;
}

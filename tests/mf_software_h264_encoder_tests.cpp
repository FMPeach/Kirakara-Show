#include "../apps/show_host/cast/cast_stage_renderer.h"
#include "../apps/show_host/cast/cast_nv12_surface_pool.h"
#include "../apps/show_host/cast/mpeg_ts_muxer.h"
#include "../apps/show_host/cast/mf_d3d11_h264_encoder.h"
#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <mfapi.h>
#include <objbase.h>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool is_annex_b(const std::uint8_t* bytes, std::size_t size) {
    return bytes && size >= 4
        && bytes[0] == 0 && bytes[1] == 0
        && (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1));
}

bool has_vcl_nal(const std::uint8_t* bytes, std::size_t size) {
    if (!bytes) return false;
    for (std::size_t i = 0; i + 4 < size; ++i) {
        std::size_t nal_offset{};
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1) {
            nal_offset = i + 3;
        } else if (i + 5 < size
                && bytes[i] == 0 && bytes[i + 1] == 0
                && bytes[i + 2] == 0 && bytes[i + 3] == 1) {
            nal_offset = i + 4;
        } else {
            continue;
        }
        const auto nal_type = bytes[nal_offset] & 0x1fU;
        if (nal_type >= 1U && nal_type <= 5U) return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    expect(SUCCEEDED(com), "CoInitializeEx failed");
    expect(SUCCEEDED(MFStartup(MF_VERSION)), "MFStartup failed");

    CastStageRenderer renderer;
    kirakara::show::win32::StageCanvas canvas{};
    canvas.width = 1920;
    canvas.height = 1080;
    canvas.frame_rate_num = 30;
    canvas.frame_rate_den = 1;
    expect(renderer.configure(canvas), "CastStageRenderer configure failed");
    expect(renderer.compose_idle_frame(), "idle Stage frame failed");
    renderer.finalize_frame();

    std::mutex mutex;
    std::condition_variable output_ready;
    bool encoded{};
    bool annex_b{};
    std::size_t encoded_bytes{};
    std::vector<std::uint8_t> transport_stream;
    MpegTsMuxer muxer;

    MfD3D11H264EncoderConfig config{};
    config.width = canvas.width;
    config.height = canvas.height;
    config.frame_rate_num = canvas.frame_rate_num;
    config.frame_rate_den = canvas.frame_rate_den;
    config.bitrate = 4000000;
    config.gop_size_frames = 15;
    config.preference = MfH264EncoderPreference::software_only;

    MfD3D11H264Encoder encoder;
    CastPipelineDiagnostics diagnostics;
    expect(encoder.start(
            renderer.native_d3d_device(),
            config,
            [&](const std::uint8_t* bytes,
                    std::size_t size,
                    std::int64_t pts90k,
                    bool keyframe) {
                if (!has_vcl_nal(bytes, size)) return;
                {
                    std::lock_guard lock(mutex);
                    muxer.append_video_access_unit(
                        bytes, size, pts90k, keyframe, transport_stream);
                    encoded = true;
                    annex_b = is_annex_b(bytes, size);
                    encoded_bytes = size;
                }
                output_ready.notify_one();
            },
            &diagnostics),
        "software H264 encoder failed to start");
    expect(encoder.backend() == MfH264EncoderBackend::software,
        "software-only preference selected the wrong backend");

    const auto duration = static_cast<std::int64_t>(10000000ULL / 30ULL);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(10);
    auto* stage_texture =
        renderer.compositor().current_frame().overlay_texture;
    expect(encoder.prepare_input_texture(stage_texture),
        "precreate fixed Stage input view");
    const auto warmed = diagnostics.snapshot();
    const auto warmed_textures = warmed.counter(
        CastPipelineCounter::texture_creations);
    const auto warmed_views = warmed.counter(
        CastPipelineCounter::view_creations);
    expect(encoder.submit_texture(stage_texture, 0, duration),
        "initial H264 texture submission failed");
    expect(encoder.submit_repeated_texture(
            stage_texture, duration, duration),
        "repeated H264 texture submission failed");
    std::int64_t frame{2};
    while (frame < 90 && std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(mutex);
            if (encoded) break;
        }
        if (encoder.submit_repeated_texture(
                stage_texture,
                frame * duration,
                duration)) {
            ++frame;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    {
        std::unique_lock lock(mutex);
        static_cast<void>(output_ready.wait_until(
            lock, deadline, [&] { return encoded; }));
    }
    const auto pipeline = diagnostics.snapshot();
    const auto textures = pipeline.counter(
        CastPipelineCounter::texture_creations);
    expect(textures >= 8 && textures <= 32,
        "encoder did not retain a fixed NV12 surface pool");
    expect(textures == warmed_textures,
        "encoder created a texture after startup prewarming");
    expect(pipeline.counter(CastPipelineCounter::video_processor_passes) == 1,
        "held frame should reuse one cached NV12 conversion");
    expect(pipeline.counter(CastPipelineCounter::view_creations)
            == warmed_views,
        "encoder created a video processor view in the frame path");
    encoder.stop();

    expect(encoded, "software H264 encoder produced no access unit");
    expect(annex_b, "software H264 output is not Annex-B");
    expect(encoded_bytes > 0, "software H264 access unit is empty");
    expect(!transport_stream.empty()
            && transport_stream.size() % MpegTsMuxer::kPacketSize == 0,
        "software H264 did not packetize into MPEG-TS");
    expect(transport_stream.front() == 0x47,
        "software H264 MPEG-TS has no sync byte");

    CastNv12SurfacePool direct_pool;
    CastNv12SurfacePoolConfig pool_config;
    pool_config.width = canvas.width;
    pool_config.height = canvas.height;
    pool_config.capacity = 2;
    pool_config.create_plane_srvs = true;
    pool_config.create_plane_uavs = false;
    pool_config.video_processor_output = false;
    pool_config.encoder_compatible = false;
    expect(direct_pool.configure(
            renderer.native_d3d_device(), pool_config),
        "direct NV12 test pool creation failed");
    auto direct_surface = direct_pool.try_acquire();
    expect(static_cast<bool>(direct_surface),
        "direct NV12 test surface acquisition failed");
    auto* direct_texture = static_cast<ID3D11Texture2D*>(
        direct_surface.compose_texture());
    std::vector<std::uint8_t> black_nv12(
        static_cast<std::size_t>(canvas.width) * canvas.height * 3U / 2U,
        16);
    std::fill(
        black_nv12.begin()
            + static_cast<std::size_t>(canvas.width) * canvas.height,
        black_nv12.end(), 128);
    ID3D11DeviceContext* context{};
    static_cast<ID3D11Device*>(renderer.native_d3d_device())
        ->GetImmediateContext(&context);
    expect(context != nullptr, "direct NV12 test context unavailable");
    context->UpdateSubresource(direct_texture, 0, nullptr,
        black_nv12.data(), canvas.width,
        static_cast<UINT>(black_nv12.size()));
    context->Release();

    {
        std::lock_guard lock(mutex);
        encoded = false;
        encoded_bytes = 0;
    }
    std::int64_t forced_keyframe_min_pts90k{
        std::numeric_limits<std::int64_t>::max()};
    bool forced_keyframe_observed{};
    config.input_format = MfH264EncoderInputFormat::nv12;
    config.gop_size_frames = 300;
    CastPipelineDiagnostics direct_diagnostics;
    expect(direct_diagnostics.set_trace_enabled(true),
        "enable direct encoder trace");
    expect(encoder.start(
            renderer.native_d3d_device(),
            config,
            [&](const std::uint8_t* bytes,
                    std::size_t size,
                    std::int64_t pts90k,
                    bool keyframe) {
                if (!has_vcl_nal(bytes, size)) return;
                {
                    std::lock_guard lock(mutex);
                    encoded = true;
                    encoded_bytes = size;
                    forced_keyframe_observed = forced_keyframe_observed
                        || (keyframe
                            && pts90k >= forced_keyframe_min_pts90k);
                }
                output_ready.notify_one();
            },
            &direct_diagnostics),
        "direct NV12 software H264 encoder failed to start");
    expect(!encoder.prepare_input_texture(stage_texture),
        "direct NV12 mode accepted a BGRA preparation call");
    expect(!encoder.submit_texture(stage_texture, 0, duration),
        "direct NV12 mode accepted a BGRA submission");

    auto direct_retention = direct_surface.retention_token();
    direct_surface.reset();
    const auto direct_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(10);
    frame = 0;
    while (frame < 90
            && std::chrono::steady_clock::now() < direct_deadline) {
        {
            std::lock_guard lock(mutex);
            if (encoded) break;
        }
        if (encoder.submit_nv12_texture(
                direct_texture,
                direct_retention,
                frame * duration,
                duration)) {
            ++frame;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    {
        std::unique_lock lock(mutex);
        static_cast<void>(output_ready.wait_until(
            lock, direct_deadline, [&] { return encoded; }));
    }
    const auto keyframe_support = encoder.force_keyframe_support();
    expect(keyframe_support != MfH264ForceKeyframeSupport::unknown,
        "running encoder did not publish force-keyframe capability");
    {
        std::lock_guard lock(mutex);
        forced_keyframe_min_pts90k = frame * duration * 9LL / 1000LL;
        forced_keyframe_observed = false;
    }
    expect(encoder.submit_nv12_texture(
            direct_texture,
            direct_retention,
            frame * duration,
            duration,
            true),
        "force-keyframe NV12 submission failed");
    ++frame;
    const auto keyframe_deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < keyframe_deadline) {
        {
            std::unique_lock lock(mutex);
            if (output_ready.wait_for(
                    lock,
                    std::chrono::milliseconds(5),
                    [&] { return forced_keyframe_observed; })) {
                break;
            }
        }
        if (encoder.submit_nv12_texture(
                direct_texture,
                direct_retention,
                frame * duration,
                duration)) {
            ++frame;
        }
    }
    encoder.stop();
    direct_retention.reset();
    expect(encoded && encoded_bytes > 0,
        "direct NV12 software H264 encoder produced no access unit");
    const auto direct_pipeline = direct_diagnostics.snapshot();
    expect(direct_pipeline.counter(
            CastPipelineCounter::texture_creations) == 0,
        "direct NV12 mode allocated an internal conversion texture");
    expect(direct_pipeline.counter(
            CastPipelineCounter::video_processor_passes) == 0,
        "direct NV12 mode invoked the BGRA video processor");
    std::array<CastPipelineTraceRecord, 256> trace{};
    const auto trace_count = direct_diagnostics.read_trace(
        trace.data(), static_cast<std::uint32_t>(trace.size()), 0);
    bool saw_keyframe_request{};
    bool saw_keyframe_request_result{};
    for (std::uint32_t index = 0; index < trace_count; ++index) {
        const auto& record = trace[index];
        if (record.event != CastPipelineEvent::encoder_process_input
                || (record.value
                    & cast_encoder_input_diagnostic::keyframe_requested)
                    == 0) {
            continue;
        }
        saw_keyframe_request = true;
        saw_keyframe_request_result = (record.value
            & (cast_encoder_input_diagnostic::keyframe_applied
                | cast_encoder_input_diagnostic::keyframe_unsupported
                | cast_encoder_input_diagnostic::keyframe_failed)) != 0;
    }
    expect(saw_keyframe_request && saw_keyframe_request_result,
        "force-keyframe request/result was not observable in diagnostics");
    if (keyframe_support == MfH264ForceKeyframeSupport::supported) {
        expect(forced_keyframe_observed,
            "supported force-keyframe request produced no clean point");
    }
    expect(direct_pool.available() == direct_pool.capacity(),
        "direct NV12 encoder retained a surface after stop");
    direct_pool.reset();

    MFShutdown();
    CoUninitialize();
    std::cout << "Software H264 encoder test passed: "
              << encoded_bytes << " bytes\n";
    return 0;
}

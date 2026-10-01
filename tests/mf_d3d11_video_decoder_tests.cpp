#include "../apps/show_host/cast/cast_stage_renderer.h"
#include "../apps/show_host/cast/cast_av_sync.h"
#include "../apps/show_host/cast/cast_video_frame_normalizer.h"
#include "../apps/show_host/cast/d3d11_cast_capabilities.h"
#include "../apps/show_host/media/mf_d3d11_video_decoder.h"
#include "../apps/show_host/stage/native_stage_video_decoder_config.h"

#include <chrono>
#include <cmath>
#include <cwchar>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

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

std::shared_ptr<const DecodedD3D11VideoFrame> wait_for_texture(
        const MfD3D11VideoDecoder& decoder,
        const std::wstring& source,
        std::uint64_t revision,
        double position) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        auto texture = decoder.texture_for(source, revision, position);
        if (texture) return texture;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

std::shared_ptr<const DecodedD3D11VideoFrame> wait_for_matching_texture(
        MfD3D11VideoDecoder& decoder,
        const std::wstring& source,
        std::uint64_t revision,
        double start_position) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const auto position = start_position
            + std::chrono::duration<double>(now - started).count();
        decoder.synchronize(
            source, revision, position, MfD3D11PlaybackState::playing);
        auto texture = decoder.texture_for(source, revision, position);
        if (texture && video_frame_matches_program_time(
                texture->timestamp_seconds(), position)) {
            return texture;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

bool wait_for_start_buffer(
        const MfD3D11VideoDecoder& decoder,
        const std::wstring& source,
        std::uint64_t revision) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = decoder.buffer_status(source, revision);
        if (status.availability
                == MfD3D11VideoAvailability::fatal_error) {
            std::cerr << "decoder entered fatal state: 0x"
                      << std::hex
                      << static_cast<std::uint32_t>(status.hresult)
                      << std::dec << '\n';
            return false;
        }
        if (status.frame_count > 0
                && (status.end_of_stream
                    || status.buffered_duration_seconds >= 0.05)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool wait_for_buffer_runway(
        const MfD3D11VideoDecoder& decoder,
        const std::wstring& source,
        std::uint64_t revision,
        double position,
        double minimum_seconds) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(15);
    MfD3D11VideoBufferStatus last_status;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = decoder.buffer_status(source, revision);
        last_status = status;
        if (status.availability
                == MfD3D11VideoAvailability::fatal_error) {
            return false;
        }
        if (status.end_of_stream) return status.frame_count > 0;
        if (status.frame_count > 0
                && status.buffered_end_seconds - position
                    >= minimum_seconds) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::cerr << "deep buffer timeout: frames=" << last_status.frame_count
              << " range=" << last_status.buffered_start_seconds
              << ".." << last_status.buffered_end_seconds
              << " duration=" << last_status.buffered_duration_seconds
              << " availability="
              << static_cast<std::uint32_t>(last_status.availability)
              << " hr=0x" << std::hex
              << static_cast<std::uint32_t>(last_status.hresult)
              << std::dec << '\n';
    for (int index = 0; index <= 10; ++index) {
        const auto sample_position = position + index * 0.025;
        const auto frame = decoder.texture_for(
            source, revision, sample_position);
        std::cerr << "  pick " << sample_position << " -> "
                  << (frame ? frame->timestamp_seconds() : -1.0)
                  << " duration="
                  << (frame ? frame->duration_seconds() : 0.0) << '\n';
    }
    return false;
}

bool wait_for_availability(
        const MfD3D11VideoDecoder& decoder,
        const std::wstring& source,
        std::uint64_t revision,
        MfD3D11VideoAvailability expected) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (decoder.buffer_status(source, revision).availability == expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    expect(cast_program_ready_for_timeline(true, 7, 7),
        "current Cast timeline should be ready");
    expect(!cast_program_ready_for_timeline(true, 7, 8),
        "previous-song Cast readiness must not release the new timeline");
    expect(!cast_program_ready_for_timeline(false, 8, 8),
        "an unpublished Cast timeline must remain gated");
    expect(video_frame_matches_program_time(1.000, 1.050),
        "50 ms video delta should be accepted");
    expect(!video_frame_matches_program_time(1.000, 1.051),
        "video delta above 50 ms should gate program audio");
    expect(std::abs(cast_audio_chunk_start_time(10.0, 2, 60, 1)
            - (10.0 - 2.0 / 60.0)) < 1e-9,
        "skipped video slots should preserve continuous audio time");
    expect(cast_audio_chunk_start_time(0.01, 2, 60, 1) == 0.0,
        "cast audio chunk start should clamp to the song origin");

    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto mf = MFStartup(MF_VERSION);
    expect(SUCCEEDED(mf), "MFStartup failed");

    CastStageRenderer renderer;
    kirakara::show::win32::StageCanvas canvas{};
    canvas.width = 320;
    canvas.height = 180;
    canvas.frame_rate_num = 30;
    canvas.frame_rate_den = 1;
    expect(renderer.configure(canvas), "CastStageRenderer configure failed");

    // A stalled progressive local source sleeps on directory changes with a
    // one-second fallback. Stop/seek/source changes use a separate wake event
    // and must not inherit that delay.
    {
        wchar_t temp_path[MAX_PATH]{};
        expect(GetTempPathW(MAX_PATH, temp_path) != 0,
            "GetTempPathW failed");
        const auto missing_source = std::wstring(temp_path)
            + L"kirakara-show-missing-"
            + std::to_wstring(GetCurrentProcessId()) + L".mp4";
        static_cast<void>(DeleteFileW(missing_source.c_str()));
        MfD3D11VideoDecoderConfig stalled_config;
        stalled_config.observe_local_source_growth = true;
        MfD3D11VideoDecoder stalled_decoder;
        expect(stalled_decoder.start(
                renderer.native_d3d_device(), stalled_config),
            "stalled decoder start failed");
        constexpr std::uint64_t stalled_revision = 77;
        stalled_decoder.synchronize(
            missing_source, stalled_revision, 0.0,
            MfD3D11PlaybackState::playing);
        expect(wait_for_availability(
                stalled_decoder, missing_source, stalled_revision,
                MfD3D11VideoAvailability::retryable_starvation),
            "missing progressive source was not reported as retryable");
        const auto stop_started = std::chrono::steady_clock::now();
        stalled_decoder.stop();
        const auto stop_elapsed = std::chrono::steady_clock::now()
            - stop_started;
        expect(stop_elapsed < std::chrono::milliseconds(500),
            "stalled progressive retry delayed decoder stop");
    }

    MfD3D11VideoDecoder decoder;
    expect(decoder.start(renderer.native_d3d_device()),
        "MfD3D11VideoDecoder start failed");
    const auto idle_status = decoder.buffer_status(L"", 0);
    expect(idle_status.availability == MfD3D11VideoAvailability::idle,
        "new decoder did not report idle availability");
    expect(idle_status.frame_count == 0
            && idle_status.buffered_duration_seconds == 0.0,
        "idle decoder reported a non-empty buffer");

    if (argc > 1) {
        const std::wstring source = argv[1];
        const double position = argc > 2
            ? std::wcstod(argv[2], nullptr) : 1.0;
        D3D11CastCapabilityProbeOptions source_probe_options;
        source_probe_options.decoder_source = source.c_str();
        source_probe_options.probe_hardware_encoder = false;
        const auto source_probe = D3D11CastCapabilityProbe::run(
            renderer.native_d3d_device(), source_probe_options);
        expect(source_probe.native_nv12_source.passed(),
            "capability probe rejected a source decoded as native NV12");
        constexpr std::uint64_t revision = 1;
        decoder.synchronize(
            source, revision, position, MfD3D11PlaybackState::paused);
        auto frame = wait_for_texture(
            decoder, source, revision, position);
        expect(frame != nullptr, "decoder did not publish a D3D11 frame");
        expect(frame->texture() != nullptr, "decoded texture is null");
        expect(frame->width() > 0 && frame->height() > 0,
            "decoded texture dimensions are invalid");
        expect(frame->generation() == revision,
            "decoded frame did not retain its timeline generation");
        expect(frame->format().valid(),
            "decoded BGRA frame did not retain output metadata");
        expect(frame->format().pixel_format
                == MfD3D11VideoPixelFormat::bgra8,
            "default decoder output format is no longer BGRA8");
        expect(renderer.compose_video_texture(
                frame->texture(), frame->width(), frame->height()),
            "StageCompositor rejected decoder output texture");
        renderer.finalize_frame();

        // Native Stage promotion exchanges complete decoder workers instead
        // of reopening the prepared source. Verify that both live sessions,
        // including their retained first frames, follow the wrapper swap and
        // can then be restored without invalidating either worker.
        {
            MfD3D11VideoDecoder prepared_decoder;
            const auto prepared_config =
                native_stage_standby_video_decoder_config();
            expect(prepared_config.max_queued_frames > 1,
                "standby config must retain active queue capacity");
            expect(prepared_config.decode_ahead_seconds > 0.0,
                "standby config must retain active decode runway");
            expect(prepared_decoder.start(
                    renderer.native_d3d_device(), prepared_config),
                "prepared decoder start failed");
            constexpr std::uint64_t prepared_revision = 90;
            prepared_decoder.synchronize(
                source, prepared_revision, position,
                MfD3D11PlaybackState::paused);
            const auto prepared_frame = wait_for_texture(
                prepared_decoder, source, prepared_revision, position);
            expect(prepared_frame != nullptr,
                "prepared decoder did not retain its first frame");

            decoder.swap(prepared_decoder);
            expect(decoder.texture_for(
                    source, prepared_revision, position) != nullptr,
                "promoted wrapper lost the prepared decoder frame");
            expect(prepared_decoder.texture_for(
                    source, revision, position) != nullptr,
                "standby wrapper lost the previous active decoder frame");

            decoder.synchronize(
                source, prepared_revision, position,
                MfD3D11PlaybackState::playing);
            expect(wait_for_buffer_runway(
                    decoder, source, prepared_revision, position, 0.05),
                "promoted decoder did not resume sequential decoding");
            const auto promoted_status = decoder.buffer_status(
                source, prepared_revision);
            expect(promoted_status.frame_count > 1,
                "promoted decoder remained trapped at its primed frame");
            const auto promoted_frame = decoder.texture_for(
                source,
                prepared_revision,
                promoted_status.buffered_end_seconds);
            expect(promoted_frame != nullptr
                    && promoted_frame->frame_id()
                        != prepared_frame->frame_id(),
                "promoted decoder did not advance beyond the primed frame");

            decoder.swap(prepared_decoder);
            prepared_decoder.stop();
        }

        decoder.synchronize(
            source, revision, position, MfD3D11PlaybackState::playing);
        expect(wait_for_start_buffer(decoder, source, revision),
            "decoder did not build a stable Cast start buffer");
        const auto playing_status = decoder.buffer_status(source, revision);
        expect(playing_status.availability
                    == MfD3D11VideoAvailability::ready
                || playing_status.availability
                    == MfD3D11VideoAvailability::end_of_stream,
            "buffered decoder did not report ready/EOS availability");
        expect(playing_status.frame_count > 0,
            "ready decoder reported an empty frame queue");
        expect(playing_status.buffered_end_seconds
                >= playing_status.buffered_start_seconds,
            "decoder reported an inverted buffered range");
        expect(std::abs(playing_status.buffered_duration_seconds
                - (playing_status.buffered_end_seconds
                    - playing_status.buffered_start_seconds)) < 1e-6,
            "decoder buffered duration disagrees with its range");
        if (playing_status.source_duration_seconds > 0.0) {
            expect(playing_status.source_duration_seconds + 0.1
                    >= playing_status.buffered_end_seconds,
                "decoder buffer extends beyond the declared source duration");
        }

        // Hiding the only native output stops Stage synchronization while the
        // playback clock keeps moving. Resuming therefore advances position
        // without changing the source or public timeline revision. The old
        // texture must be invalidated and the MF session must seek instead of
        // decoding every skipped frame in sequence.
        // Stay inside any source that was already able to provide the initial
        // frame. The old hard-coded 10 s target made the manual smoke fail on
        // the repository's intentionally short 8 s fixtures.
        const double resume_position = position >= 0.5
            ? position - 0.5 : position + 0.5;
        decoder.synchronize(source, revision, resume_position,
            MfD3D11PlaybackState::playing);
        const auto immediate = decoder.texture_for(
            source, revision, resume_position);
        expect(!immediate || video_frame_matches_program_time(
                immediate->timestamp_seconds(), resume_position),
            "same-revision resume exposed a stale decoder texture");
        frame = wait_for_matching_texture(
            decoder, source, revision, resume_position);
        expect(frame != nullptr,
            "decoder did not recover from a same-revision position jump");

        constexpr std::uint64_t seek_revision = revision + 1;
        decoder.synchronize(source, seek_revision, position,
            MfD3D11PlaybackState::paused);
        expect(decoder.texture_for(source, seek_revision, position) == nullptr,
            "seek revision reused a stale decoder texture");
        frame = wait_for_texture(decoder, source, seek_revision, position);
        expect(frame != nullptr, "decoder did not recover after seek");

        decoder.synchronize(source, seek_revision, position,
            MfD3D11PlaybackState::idle);
        expect(decoder.texture_for(source, seek_revision, position) == nullptr,
            "idle decoder retained a visible texture");
        expect(decoder.buffer_status(source, seek_revision).availability
                == MfD3D11VideoAvailability::idle,
            "idle synchronization retained a non-idle decoder status");

        decoder.stop();

        // Model the first completed cache publish without polling: the path
        // does not exist when Source Reader opens it, then a downloader makes
        // the completed file visible in one operation. Directory change
        // notification should wake the retry well before the one-second
        // fallback expires.
        wchar_t temp_path[MAX_PATH]{};
        expect(GetTempPathW(MAX_PATH, temp_path) != 0,
            "GetTempPathW failed for growth recovery test");
        const auto growing_source = std::wstring(temp_path)
            + L"kirakara-show-growing-"
            + std::to_wstring(GetCurrentProcessId()) + L".mp4";
        static_cast<void>(DeleteFileW(growing_source.c_str()));
        MfD3D11VideoDecoderConfig growing_config;
        growing_config.observe_local_source_growth = true;
        MfD3D11VideoDecoder growing_decoder;
        expect(growing_decoder.start(
                renderer.native_d3d_device(), growing_config),
            "growing decoder start failed");
        constexpr std::uint64_t growing_revision = 10;
        growing_decoder.synchronize(
            growing_source, growing_revision, position,
            MfD3D11PlaybackState::playing);
        expect(wait_for_availability(
                growing_decoder, growing_source, growing_revision,
                MfD3D11VideoAvailability::retryable_starvation),
            "not-yet-published cache file was not retryable");
        expect(CopyFileW(source.c_str(), growing_source.c_str(), FALSE),
            "failed to publish completed cache file");
        const auto growth_started = std::chrono::steady_clock::now();
        auto grown_frame = wait_for_texture(
            growing_decoder, growing_source, growing_revision, position);
        const auto growth_elapsed = std::chrono::steady_clock::now()
            - growth_started;
        expect(grown_frame != nullptr,
            "decoder did not recover after cache file publication");
        expect(growth_elapsed < std::chrono::milliseconds(900),
            "cache publication waited for the fallback retry timer");
        growing_decoder.stop();
        static_cast<void>(DeleteFileW(growing_source.c_str()));

        MfD3D11VideoDecoder nv12_decoder;
        MfD3D11VideoDecoderConfig nv12_config;
        nv12_config.output_format = MfD3D11VideoPixelFormat::nv12;
        nv12_config.max_queued_frames = 16;
        nv12_config.max_queued_bytes = 256U * 1024U * 1024U;
        nv12_config.decode_ahead_seconds = 0.25;
        expect(nv12_decoder.start(
                renderer.native_d3d_device(), nv12_config),
            "native NV12 decoder start failed");
        constexpr std::uint64_t nv12_revision = 11;
        nv12_decoder.synchronize(source, nv12_revision, position,
            MfD3D11PlaybackState::paused);
        auto nv12_frame = wait_for_texture(
            nv12_decoder, source, nv12_revision, position);
        expect(nv12_frame != nullptr,
            "decoder did not publish a native NV12 frame");
        expect(nv12_frame->format().valid(),
            "native NV12 frame did not retain output metadata");
        expect(nv12_frame->format().pixel_format
                == MfD3D11VideoPixelFormat::nv12,
            "native NV12 frame reported the wrong pixel format");
        expect(nv12_frame->format().width == nv12_frame->width()
                && nv12_frame->format().height == nv12_frame->height(),
            "native NV12 metadata dimensions differ from the texture");

        auto* nv12_texture = static_cast<ID3D11Texture2D*>(
            nv12_frame->texture());
        D3D11_TEXTURE2D_DESC nv12_desc{};
        nv12_texture->GetDesc(&nv12_desc);
        expect(nv12_desc.Format == DXGI_FORMAT_NV12,
            "native decoder output texture is not DXGI_FORMAT_NV12");
        ID3D11Device* nv12_device{};
        nv12_texture->GetDevice(&nv12_device);
        expect(nv12_device == renderer.native_d3d_device(),
            "native NV12 texture was allocated on a different D3D device");
        if (nv12_device) nv12_device->Release();

        nv12_decoder.synchronize(source, nv12_revision, position,
            MfD3D11PlaybackState::playing);
        expect(wait_for_buffer_runway(
                nv12_decoder, source, nv12_revision, position, 0.12),
            "Cast-configured decoder did not build a deep bounded runway");
        const auto deep_status = nv12_decoder.buffer_status(
            source, nv12_revision);
        expect(deep_status.frame_count <= nv12_config.max_queued_frames,
            "Cast-configured decoder exceeded its frame capacity");

        CastVideoFrameNormalizer normalizer;
        CastVideoFrameNormalizerConfig normalize_config;
        normalize_config.create_plane_srvs = true;
        normalize_config.allow_direct_passthrough = true;
        expect(normalizer.configure(
                renderer.native_d3d_device(), normalize_config),
            "configure normalizer for decoder output");
        CastVideoFrameInput normalize_input;
        normalize_input.texture = nv12_frame->texture();
        normalize_input.format = nv12_frame->format();
        normalize_input.timestamp_seconds =
            nv12_frame->timestamp_seconds();
        normalize_input.duration_seconds = nv12_frame->duration_seconds();
        normalize_input.frame_id = nv12_frame->frame_id();
        normalize_input.generation = nv12_frame->generation();
        normalize_input.retention = nv12_frame->retention_token();
        const auto normalized = normalizer.normalize(normalize_input);
        if (!normalized) {
            std::cerr << "normalizer failure code: "
                      << static_cast<std::uint32_t>(
                          normalizer.last_failure())
                      << ", HRESULT: 0x" << std::hex
                      << static_cast<std::uint32_t>(
                          normalizer.last_hresult()) << std::dec << '\n';
        }
        expect(normalized && normalized->texture(),
            "normalizer rejected native decoder NV12 output");
        expect(normalized->format().width == 1920
                && normalized->format().height == 1080,
            "decoder output was not normalized to fixed Cast dimensions");
        expect(normalizer.normalize(normalize_input) == normalized,
            "normalizer did not cache the native decoder source frame");
        const auto geometry = cast_calculate_video_normalize_geometry(
            normalize_input.format,
            normalize_config.width,
            normalize_config.height);
        if (geometry && geometry->direct_copy) {
            expect(normalized->texture() == nv12_frame->texture(),
                "exact decoder output was copied instead of borrowed");
            expect(normalized->y_srv() && normalized->uv_srv(),
                "borrowed decoder output is missing NV12 plane views");
            expect(normalized->retention_token()
                    == normalize_input.retention,
                "borrowed decoder output lost its pool retention token");
        }
        auto* retained_texture = static_cast<ID3D11Texture2D*>(
            normalized->texture());
        normalizer.reset();
        nv12_frame.reset();
        nv12_decoder.stop();
        D3D11_TEXTURE2D_DESC retained_desc{};
        retained_texture->GetDesc(&retained_desc);
        expect(retained_desc.Format == DXGI_FORMAT_NV12,
            "normalized frame did not retain its texture across teardown");
    }

    decoder.suspend();
    decoder.stop();
    if (SUCCEEDED(mf)) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}

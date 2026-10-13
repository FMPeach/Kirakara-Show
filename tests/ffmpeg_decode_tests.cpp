#include "../apps/show_host/media/ffmpeg_decode.h"
#include "../apps/show_host/audio/audio_decode.h"
#include "../apps/show_host/audio/decoded_buffer.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

#include <d3d11.h>
#include <d3d10_1.h>
#include <mfapi.h>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

void decode_video(const std::wstring& path, MfD3D11VideoPixelFormat format,
        ID3D11Device* device, bool require_hardware = false) {
    FfmpegVideoDecoder decoder;
    expect(decoder.open(path, format, false, device), "open video");
    expect(std::abs(decoder.duration() - 2.0) < 0.1, "video duration");
    FfmpegVideoFrame frame;
    int count = 0, hardware_count = 0;
    double previous = -1.0;
    for (;;) {
        const auto status = decoder.read(frame);
        if (status == FfmpegReadStatus::end_of_stream) break;
        expect(status == FfmpegReadStatus::frame, "decode video frame");
        expect(frame.format.width == 640 && frame.format.height == 360, "video dimensions");
        expect(frame.timestamp_seconds > previous, "monotonic video timestamps");
        expect(std::abs(frame.timestamp_seconds - count / 30.0) < 0.002, "video presentation order");
        expect(std::abs(frame.duration_seconds - 1.0 / 30.0) < 0.002, "video frame duration");
        previous = frame.timestamp_seconds;
        if (frame.hardware_texture) {
            ++hardware_count;
            auto* texture = static_cast<ID3D11Texture2D*>(frame.hardware_texture);
            ID3D11Device* owner{};
            texture->GetDevice(&owner);
            expect(owner == device, "hardware decoder uses renderer device");
            owner->Release();
            expect(frame.hardware_retention != nullptr, "hardware frame retention");
        } else {
            expect(frame.pixels.size() == (format == MfD3D11VideoPixelFormat::nv12
                ? 640U * 360U * 3U / 2U : 640U * 360U * 4U), "CPU frame layout");
        }
        ++count;
        expect(count <= 60, "decoder terminates");
    }
    expect(count == 60, "drain delayed frames at EOF");
    if (require_hardware) expect(hardware_count == count, "expected hardware frames on this device");
    expect(decoder.seek(0.8), "seek after EOF");
    expect(decoder.read(frame) == FfmpegReadStatus::frame, "decode after seek");
    expect(frame.timestamp_seconds <= 0.8 && frame.timestamp_seconds >= 0.0, "seek to previous keyframe");
    expect(decoder.seek(0.0), "backward seek");
    expect(decoder.read(frame) == FfmpegReadStatus::frame, "decode after backward seek");
    expect(std::abs(frame.timestamp_seconds) < 0.002, "backward seek resets timestamps");
    auto retention = frame.hardware_retention;
    auto* retained_texture = static_cast<ID3D11Texture2D*>(frame.hardware_texture);
    decoder.close();
    if (retained_texture) {
        D3D11_TEXTURE2D_DESC desc{};
        retained_texture->GetDesc(&desc);
        expect(desc.Width >= 640 && desc.Height >= 360, "frame survives decoder close");
    }
    std::wcout << std::filesystem::path(path).filename().wstring() << L": " << count
        << L" frames, " << hardware_count << L" hardware frames\n";
}

std::shared_ptr<const DecodedD3D11VideoFrame> wait_frame(MfD3D11VideoDecoder& decoder,
        const std::wstring& source, std::uint64_t revision, double position) {
    decoder.synchronize(source, revision, position, MfD3D11PlaybackState::paused);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        auto frame = decoder.texture_for(source, revision, position);
        if (frame && std::abs(frame->timestamp_seconds() - position) < 0.04) return frame;
        const auto status = decoder.buffer_status(source, revision);
        expect(status.availability != MfD3D11VideoAvailability::fatal_error, "integrated decoder failure");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(false, "integrated decoder timeout");
    return {};
}

void test_upload(const std::wstring& source, MfD3D11VideoPixelFormat format,
        ID3D11Device* device) {
    MfD3D11VideoDecoder decoder;
    MfD3D11VideoDecoderConfig config;
    config.output_format = format;
    expect(decoder.start(device, config), "start integrated decoder");
    auto frame = wait_frame(decoder, source, 1, 0.0);
    auto* texture = static_cast<ID3D11Texture2D*>(frame->texture());
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    expect(desc.Width == 640 && desc.Height == 360, "uploaded texture dimensions");
    expect(desc.Format == (format == MfD3D11VideoPixelFormat::nv12
        ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM), "uploaded texture format");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D* staging{};
    expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "readback texture");
    ID3D11DeviceContext* context{};
    device->GetImmediateContext(&context);
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    expect(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), "readback decoded frame");
    const auto* pixels = static_cast<unsigned char*>(mapped.pData);
    unsigned int minimum = 255, maximum = 0;
    const auto stride = format == MfD3D11VideoPixelFormat::nv12 ? 1U : 4U;
    for (unsigned int y = 0; y < 360; ++y) {
        for (unsigned int x = 0; x < 640; ++x) {
            const auto value = pixels[y * mapped.RowPitch + x * stride];
            minimum = std::min(minimum, static_cast<unsigned int>(value));
            maximum = std::max(maximum, static_cast<unsigned int>(value));
        }
    }
    expect(maximum > minimum + 50, "decoded texture contains the test pattern");
    context->Unmap(staging, 0);
    staging->Release();
    context->Release();
    auto sought = wait_frame(decoder, source, 1, 0.8);
    expect(sought->timestamp_seconds() > frame->timestamp_seconds() + 0.7,
        "seek replaces the published frame");
    sought = wait_frame(decoder, source, 2, 0.0);
    expect(sought->timestamp_seconds() < 0.04, "source reload resets playback");
    expect(sought->generation() != frame->generation(), "source reload changes generation");
    decoder.stop();
    texture->GetDesc(&desc);
    expect(desc.Width == 640, "output texture survives decoder stop");
}
}

int wmain(int argc, wchar_t** argv) {
    expect(argc == 2 || argc == 3, "pass the codec fixture directory and optionally --require-hardware");
    const bool require_hardware = argc == 3 && std::wstring_view(argv[2]) == L"--require-hardware";
    const std::filesystem::path directory(argv[1]);
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    expect(SUCCEEDED(MFStartup(MF_VERSION)), "Media Foundation startup");
    ID3D11Device* device{};
    D3D_FEATURE_LEVEL level{};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, nullptr, 0, D3D11_SDK_VERSION, &device, &level, nullptr)), "create renderer device");
    ID3D11DeviceContext* context{};
    device->GetImmediateContext(&context);
    ID3D10Multithread* multithread{};
    expect(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&multithread))), "multithreaded context");
    multithread->SetMultithreadProtected(TRUE);
    multithread->Release();
    context->Release();

    for (const auto* name : {L"av1-8.mkv", L"av1-10.mkv", L"hevc-8.mp4", L"hevc-10.mp4",
            L"h264-8.mp4", L"h264-10.mp4", L"h264-aac.mp4", L"视频与音频.webm"}) {
        const auto source = (directory / name).wstring();
        for (const auto format : {MfD3D11VideoPixelFormat::bgra8, MfD3D11VideoPixelFormat::nv12}) {
            decode_video(source, format, nullptr); // Software fallback is mandatory.
            decode_video(source, format, device, require_hardware
                && std::wstring_view(name) != L"h264-10.mp4"); // Hi10P may require software.
            test_upload(source, format, device);   // Actual Stage/Cast texture path.
        }
    }
    ID3D11Device* warp{};
    expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &warp, &level, nullptr)), "create device without hardware decoding");
    // Exercise rejection of a requested hardware device, not just device=null.
    for (const auto* name : {L"av1-8.mkv", L"hevc-8.mp4"})
        decode_video((directory / name).wstring(), MfD3D11VideoPixelFormat::nv12, warp);
    warp->Release();
    for (const auto* name : {L"opus-1.ogg", L"opus-2.ogg", L"opus-6.ogg", L"视频与音频.webm"}) {
        DecodedAudioBuffer audio;
        expect(decode_audio_to_float_pcm((directory / name).wstring(), audio), "decode Opus through playback entry");
        expect(audio.channels == 2 && audio.sample_rate == 48000, "Opus output format");
        expect(audio.frame_count() == 96000, "Opus pre-skip and final padding");
        double energy = 0.0;
        for (const auto sample : audio.samples) {
            expect(std::isfinite(sample), "finite audio samples");
            energy += sample * sample;
        }
        expect(energy > 1.0, "Opus is audible");
    }
    DecodedAudioBuffer missing{48000, 2, {1.0f}};
    expect(!ffmpeg_decode_audio((directory / L"missing.ogg").wstring(), missing)
        && missing.samples.empty(), "failed audio decode clears output");
    FfmpegVideoDecoder invalid;
    expect(!invalid.open((directory / L"missing.mkv").wstring(), MfD3D11VideoPixelFormat::nv12), "missing video fails");
    device->Release();
    MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    std::cout << "AV1/HEVC hardware, fallback, texture, seek and Opus tests passed\n";
    return 0;
}

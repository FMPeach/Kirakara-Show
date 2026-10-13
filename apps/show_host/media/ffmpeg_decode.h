#pragma once

#include "mf_d3d11_video_decoder.h"

#include <memory>
#include <string>
#include <vector>

struct DecodedAudioBuffer;

// D3D11VA frames retain their decoder surface until copied/converted on GPU.
// Software fallback pixels are uploaded into the existing leased texture pool.
struct FfmpegVideoFrame {
    MfD3D11VideoFormat format;
    std::vector<unsigned char> pixels;
    unsigned int row_pitch{};
    double timestamp_seconds{};
    double duration_seconds{};
    void* hardware_texture{};
    unsigned int hardware_subresource{};
    bool hardware_full_range{};
    std::shared_ptr<void> hardware_retention;
};

enum class FfmpegReadStatus { frame, end_of_stream, retryable_error, fatal_error };

class FfmpegVideoDecoder {
public:
    FfmpegVideoDecoder();
    ~FfmpegVideoDecoder();
    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    // All codecs use FFmpeg. Pass the renderer device to prefer D3D11VA.
    // restricted=true is retained for tests that specifically select AV1/HEVC.
    bool open(const std::wstring& source, MfD3D11VideoPixelFormat output,
              bool restricted = false, void* d3d11_device = nullptr);
    void close();
    bool seek(double seconds);
    FfmpegReadStatus read(FfmpegVideoFrame& frame);
    double duration() const noexcept;
    std::uint64_t format_change_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool ffmpeg_decode_audio(const std::wstring& source, DecodedAudioBuffer& buffer);

struct FfmpegMediaInfo {
    double duration_seconds{};
    bool has_audio{};
};
// Metadata for the independent playback clock and embedded audio track.
bool ffmpeg_probe_video(const std::wstring& source, FfmpegMediaInfo& info);

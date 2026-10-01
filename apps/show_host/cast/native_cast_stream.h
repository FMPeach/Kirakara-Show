#pragma once

#include "mpeg_ts_muxer.h"
#include "mpeg_ts_stream_buffer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class CastPipelineDiagnostics;

class NativeCastStream {
public:
    using Cursor = MpegTsStreamBuffer::Cursor;
    using ReadResult = MpegTsStreamBuffer::ReadResult;

    explicit NativeCastStream(
        std::size_t capacity_bytes = 8U * 1024U * 1024U);

    NativeCastStream(const NativeCastStream&) = delete;
    NativeCastStream& operator=(const NativeCastStream&) = delete;

    [[nodiscard]] Cursor cursor_from_head() const;
    [[nodiscard]] Cursor cursor_from_tail() const;
    [[nodiscard]] Cursor cursor_from_latest_keyframe() const;
    [[nodiscard]] bool rebase_to_latest_keyframe_if_lagging(
        Cursor& cursor,
        std::size_t max_lag_bytes) const;

    void reset();

    void append_h264_access_unit(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool keyframe);

    void append_aac_adts_frame(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k);

    [[nodiscard]] ReadResult read(
        Cursor& cursor,
        std::uint8_t* output,
        std::size_t max_bytes,
        std::chrono::milliseconds timeout);

    [[nodiscard]] std::size_t buffered_bytes() const;

    void set_diagnostics(CastPipelineDiagnostics* diagnostics) noexcept;
    void record_http_first_byte(
        std::uint64_t correlation_id,
        std::uint64_t connection_id,
        std::size_t bytes) noexcept;
    void record_http_send(
        std::uint64_t correlation_id,
        std::uint64_t connection_id,
        std::size_t bytes,
        std::uint64_t previous_finished_qpc,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc) noexcept;

private:
    mutable std::mutex muxer_mutex_;
    MpegTsMuxer muxer_;
    MpegTsStreamBuffer stream_;
    Cursor latest_keyframe_cursor_{};
    std::vector<std::uint8_t> h264_parameter_sets_;
    std::atomic<CastPipelineDiagnostics*> diagnostics_{};
};

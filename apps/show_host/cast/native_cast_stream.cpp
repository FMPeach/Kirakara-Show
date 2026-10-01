#include "native_cast_stream.h"

#include "cast_pipeline_diagnostics.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace {

std::size_t start_code_size(
        const std::uint8_t* data, std::size_t size, std::size_t offset) {
    if (!data || offset + 3 > size || data[offset] != 0
            || data[offset + 1] != 0) {
        return 0;
    }
    if (data[offset + 2] == 1) return 3;
    if (offset + 4 <= size && data[offset + 2] == 0
            && data[offset + 3] == 1) {
        return 4;
    }
    return 0;
}

std::size_t find_start_code(
        const std::uint8_t* data, std::size_t size, std::size_t offset) {
    for (auto i = offset; i + 3 <= size; ++i) {
        if (start_code_size(data, size, i) != 0) return i;
    }
    return size;
}

std::vector<std::uint8_t> extract_parameter_sets(
        const std::uint8_t* data,
        std::size_t size,
        bool& has_sps,
        bool& has_pps) {
    std::vector<std::uint8_t> result;
    has_sps = false;
    has_pps = false;
    auto start = find_start_code(data, size, 0);
    while (start < size) {
        const auto prefix = start_code_size(data, size, start);
        const auto nal = start + prefix;
        const auto next = find_start_code(data, size, nal + 1);
        if (nal < size) {
            const auto type = data[nal] & 0x1FU;
            if (type == 7 || type == 8) {
                has_sps = has_sps || type == 7;
                has_pps = has_pps || type == 8;
                result.insert(result.end(), data + start, data + next);
            }
        }
        start = next;
    }
    return result;
}

}  // namespace

NativeCastStream::NativeCastStream(std::size_t capacity_bytes)
        : stream_(capacity_bytes) {}

NativeCastStream::Cursor NativeCastStream::cursor_from_head() const {
    return stream_.cursor_from_head();
}

NativeCastStream::Cursor NativeCastStream::cursor_from_tail() const {
    return stream_.cursor_from_tail();
}

NativeCastStream::Cursor NativeCastStream::cursor_from_latest_keyframe() const {
    std::lock_guard<std::mutex> lock(muxer_mutex_);
    if (latest_keyframe_cursor_.generation != stream_.generation()) {
        return stream_.cursor_from_tail();
    }
    return latest_keyframe_cursor_;
}

bool NativeCastStream::rebase_to_latest_keyframe_if_lagging(
        Cursor& cursor, std::size_t max_lag_bytes) const {
    std::lock_guard<std::mutex> lock(muxer_mutex_);
    const auto record_rebase = [this] {
        if (auto* diagnostics = diagnostics_.load(
                std::memory_order_relaxed)) {
            diagnostics->increment(CastPipelineCounter::client_rebases);
        }
    };
    if (latest_keyframe_cursor_.generation == 0) return false;
    if (cursor.generation != latest_keyframe_cursor_.generation) {
        cursor = latest_keyframe_cursor_;
        record_rebase();
        return true;
    }
    if (latest_keyframe_cursor_.sequence > cursor.sequence
            && latest_keyframe_cursor_.sequence - cursor.sequence
                > static_cast<std::uint64_t>(max_lag_bytes)) {
        cursor = latest_keyframe_cursor_;
        record_rebase();
        return true;
    }
    return false;
}

void NativeCastStream::reset() {
    std::lock_guard<std::mutex> lock(muxer_mutex_);
    muxer_.reset();
    stream_.reset();
    latest_keyframe_cursor_ = {};
    h264_parameter_sets_.clear();
}

void NativeCastStream::append_h264_access_unit(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool keyframe) {
    if (!data || size == 0) return;

    std::lock_guard<std::mutex> lock(muxer_mutex_);
    const std::uint8_t* payload = data;
    std::size_t payload_size = size;
    std::vector<std::uint8_t> prefixed;
    if (keyframe) {
        bool has_sps{};
        bool has_pps{};
        auto parameters = extract_parameter_sets(
            data, size, has_sps, has_pps);
        if (has_sps && has_pps) {
            h264_parameter_sets_ = std::move(parameters);
        } else if (!h264_parameter_sets_.empty()) {
            prefixed.reserve(h264_parameter_sets_.size() + size);
            prefixed.insert(
                prefixed.end(),
                h264_parameter_sets_.begin(),
                h264_parameter_sets_.end());
            prefixed.insert(prefixed.end(), data, data + size);
            payload = prefixed.data();
            payload_size = prefixed.size();
        }
    }
    std::vector<std::uint8_t> ts;
    muxer_.append_video_access_unit(
        payload, payload_size, pts90k, keyframe, ts);
    if (!ts.empty()) {
        const auto ts_size = ts.size();
        const auto cursor = stream_.append(std::move(ts));
        if (keyframe) latest_keyframe_cursor_ = cursor;
        if (auto* diagnostics = diagnostics_.load(
                std::memory_order_relaxed)) {
            diagnostics->record(CastPipelineEvent::ts_append,
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, pts90k)),
                0,
                static_cast<std::int64_t>(ts_size));
        }
    }
}

void NativeCastStream::append_aac_adts_frame(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k) {
    if (!data || size == 0) return;

    std::lock_guard<std::mutex> lock(muxer_mutex_);
    std::vector<std::uint8_t> ts;
    muxer_.append_audio_frame(data, size, pts90k, ts);
    if (!ts.empty()) {
        const auto ts_size = ts.size();
        static_cast<void>(stream_.append(std::move(ts)));
        if (auto* diagnostics = diagnostics_.load(
                std::memory_order_relaxed)) {
            diagnostics->record(CastPipelineEvent::ts_append,
                static_cast<std::uint64_t>(std::max<std::int64_t>(0, pts90k)),
                1,
                static_cast<std::int64_t>(ts_size));
        }
    }
}

NativeCastStream::ReadResult NativeCastStream::read(
        Cursor& cursor,
        std::uint8_t* output,
        std::size_t max_bytes,
        std::chrono::milliseconds timeout) {
    return stream_.read(cursor, output, max_bytes, timeout);
}

std::size_t NativeCastStream::buffered_bytes() const {
    return stream_.buffered_bytes();
}

void NativeCastStream::set_diagnostics(
        CastPipelineDiagnostics* diagnostics) noexcept {
    diagnostics_.store(diagnostics, std::memory_order_release);
}

void NativeCastStream::record_http_first_byte(
        std::uint64_t correlation_id,
        std::uint64_t connection_id,
        std::size_t bytes) noexcept {
    if (auto* diagnostics = diagnostics_.load(std::memory_order_acquire)) {
        diagnostics->record(
            CastPipelineEvent::http_first_byte,
            correlation_id,
            connection_id,
            static_cast<std::int64_t>(bytes));
    }
}

void NativeCastStream::record_http_send(
        std::uint64_t correlation_id,
        std::uint64_t connection_id,
        std::size_t bytes,
        std::uint64_t previous_finished_qpc,
        std::uint64_t started_qpc,
        std::uint64_t finished_qpc) noexcept {
    if (auto* diagnostics = diagnostics_.load(std::memory_order_acquire)) {
        diagnostics->record_flow_duration(
            CastPipelineEvent::http_send,
            correlation_id,
            previous_finished_qpc,
            started_qpc,
            finished_qpc,
            connection_id,
            static_cast<std::int64_t>(bytes));
    }
}

#include "mpeg_ts_stream_buffer.h"

#include <algorithm>
#include <utility>

MpegTsStreamBuffer::MpegTsStreamBuffer(std::size_t capacity_bytes)
        : capacity_bytes_(std::max<std::size_t>(1, capacity_bytes)) {}

MpegTsStreamBuffer::Cursor MpegTsStreamBuffer::cursor_from_head() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Cursor{.generation = generation_, .sequence = base_sequence_};
}

MpegTsStreamBuffer::Cursor MpegTsStreamBuffer::cursor_from_tail() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Cursor{.generation = generation_, .sequence = next_sequence_};
}

void MpegTsStreamBuffer::reset() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chunks_.clear();
        base_sequence_ = 0;
        next_sequence_ = 0;
        buffered_bytes_ = 0;
        ++generation_;
        if (generation_ == 0) generation_ = 1;
    }
    data_available_.notify_all();
}

MpegTsStreamBuffer::Cursor MpegTsStreamBuffer::append(
        const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) return cursor_from_tail();
    return append(std::vector<std::uint8_t>(data, data + size));
}

MpegTsStreamBuffer::Cursor MpegTsStreamBuffer::append(
        std::vector<std::uint8_t> data) {
    if (data.empty()) return cursor_from_tail();

    Chunk chunk;
    chunk.data = std::move(data);

    Cursor cursor;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        chunk.sequence = next_sequence_;
        cursor = Cursor{
            .generation = generation_,
            .sequence = chunk.sequence,
        };
        next_sequence_ += static_cast<std::uint64_t>(chunk.data.size());
        buffered_bytes_ += chunk.data.size();
        chunks_.push_back(std::move(chunk));
        trim_locked();
    }
    data_available_.notify_all();
    return cursor;
}

MpegTsStreamBuffer::ReadResult MpegTsStreamBuffer::read(
        Cursor& cursor,
        std::uint8_t* output,
        std::size_t max_bytes,
        std::chrono::milliseconds timeout) {
    ReadResult result;
    if (!output || max_bytes == 0) {
        result.timed_out = true;
        return result;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    normalize_cursor_locked(cursor, result.reset);

    if (cursor.sequence >= next_sequence_) {
        if (result.reset) return result;
        const auto ready = data_available_.wait_for(lock, timeout, [&] {
            return cursor.generation != generation_
                || cursor.sequence < base_sequence_
                || cursor.sequence < next_sequence_;
        });
        if (!ready) {
            result.timed_out = true;
            return result;
        }
        normalize_cursor_locked(cursor, result.reset);
        if (cursor.sequence >= next_sequence_) return result;
    }

    std::size_t copied = 0;
    for (const auto& chunk : chunks_) {
        const auto chunk_end =
            chunk.sequence + static_cast<std::uint64_t>(chunk.data.size());
        if (chunk_end <= cursor.sequence) continue;

        const auto offset = cursor.sequence > chunk.sequence
            ? static_cast<std::size_t>(cursor.sequence - chunk.sequence)
            : 0U;
        const auto available = chunk.data.size() - offset;
        const auto wanted = std::min(max_bytes - copied, available);
        std::copy_n(chunk.data.data() + offset, wanted, output + copied);
        copied += wanted;
        cursor.sequence += static_cast<std::uint64_t>(wanted);
        if (copied == max_bytes) break;
    }

    result.bytes = copied;
    return result;
}

std::size_t MpegTsStreamBuffer::buffered_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return buffered_bytes_;
}

std::uint64_t MpegTsStreamBuffer::generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

std::uint64_t MpegTsStreamBuffer::next_sequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_sequence_;
}

void MpegTsStreamBuffer::normalize_cursor_locked(
        Cursor& cursor, bool& reset) const {
    if (cursor.generation != generation_) {
        cursor.generation = generation_;
        cursor.sequence = base_sequence_;
        reset = true;
        return;
    }
    if (cursor.sequence < base_sequence_) {
        cursor.sequence = base_sequence_;
        reset = true;
    } else if (cursor.sequence > next_sequence_) {
        cursor.sequence = next_sequence_;
        reset = true;
    }
}

void MpegTsStreamBuffer::trim_locked() {
    while (buffered_bytes_ > capacity_bytes_ && !chunks_.empty()) {
        const auto overflow = buffered_bytes_ - capacity_bytes_;
        auto& front = chunks_.front();
        if (front.data.size() <= overflow) {
            buffered_bytes_ -= front.data.size();
            base_sequence_ = front.sequence
                + static_cast<std::uint64_t>(front.data.size());
            chunks_.pop_front();
            continue;
        }

        front.data.erase(front.data.begin(),
            front.data.begin() + static_cast<std::ptrdiff_t>(overflow));
        front.sequence += static_cast<std::uint64_t>(overflow);
        buffered_bytes_ -= overflow;
        base_sequence_ = front.sequence;
    }

    if (chunks_.empty()) {
        base_sequence_ = next_sequence_;
    }
}

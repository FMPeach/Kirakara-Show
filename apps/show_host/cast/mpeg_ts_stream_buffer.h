#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

class MpegTsStreamBuffer {
public:
    struct Cursor {
        std::uint64_t generation{};
        std::uint64_t sequence{};
    };

    struct ReadResult {
        std::size_t bytes{};
        bool reset{};
        bool timed_out{};
    };

    explicit MpegTsStreamBuffer(
        std::size_t capacity_bytes = 4U * 1024U * 1024U);

    MpegTsStreamBuffer(const MpegTsStreamBuffer&) = delete;
    MpegTsStreamBuffer& operator=(const MpegTsStreamBuffer&) = delete;

    [[nodiscard]] Cursor cursor_from_head() const;
    [[nodiscard]] Cursor cursor_from_tail() const;

    void reset();
    [[nodiscard]] Cursor append(
        const std::uint8_t* data, std::size_t size);
    [[nodiscard]] Cursor append(std::vector<std::uint8_t> data);

    [[nodiscard]] ReadResult read(
        Cursor& cursor,
        std::uint8_t* output,
        std::size_t max_bytes,
        std::chrono::milliseconds timeout);

    [[nodiscard]] std::size_t buffered_bytes() const;
    [[nodiscard]] std::uint64_t generation() const;
    [[nodiscard]] std::uint64_t next_sequence() const;

private:
    struct Chunk {
        std::uint64_t sequence{};
        std::vector<std::uint8_t> data;
    };

    void normalize_cursor_locked(Cursor& cursor, bool& reset) const;
    void trim_locked();

    std::size_t capacity_bytes_;
    mutable std::mutex mutex_;
    std::condition_variable data_available_;
    std::deque<Chunk> chunks_;
    std::uint64_t base_sequence_{};
    std::uint64_t next_sequence_{};
    std::uint64_t generation_{1};
    std::size_t buffered_bytes_{};
};

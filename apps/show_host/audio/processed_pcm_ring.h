#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

struct ProcessedPcmFormat {
    int sample_rate{};
    int channels{};

    [[nodiscard]] bool valid() const noexcept {
        return sample_rate > 0 && channels > 0;
    }
};

class ProcessedPcmRing {
public:
    void configure(int sample_rate, int channels,
            double capacity_seconds = 3.0) {
        std::lock_guard lock(mutex_);
        format_.sample_rate = sample_rate;
        format_.channels = channels;
        if (!format_.valid() || capacity_seconds <= 0.0) {
            reset_locked();
            return;
        }
        capacity_frames_ = std::max<std::size_t>(
            1, static_cast<std::size_t>(sample_rate * capacity_seconds));
        buffer_.assign(capacity_frames_ * static_cast<std::size_t>(channels), 0);
        read_frame_ = 0;
        write_frame_ = 0;
        available_frames_ = 0;
        timeline_first_frame_ = 0;
        timeline_valid_ = false;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        read_frame_ = 0;
        write_frame_ = 0;
        available_frames_ = 0;
        timeline_first_frame_ = 0;
        timeline_valid_ = false;
    }

    void reset() {
        std::lock_guard lock(mutex_);
        reset_locked();
    }

    [[nodiscard]] ProcessedPcmFormat format() const {
        std::lock_guard lock(mutex_);
        return format_;
    }

    [[nodiscard]] std::size_t available_frames() const {
        std::lock_guard lock(mutex_);
        return available_frames_;
    }

    void push(const std::int16_t* samples, std::size_t frames) {
        if (!samples || frames == 0) return;
        std::lock_guard lock(mutex_);
        if (!format_.valid() || capacity_frames_ == 0 || buffer_.empty()) {
            return;
        }
        timeline_valid_ = false;
        append_locked(samples, frames);
    }

    // Stores final processed PCM at its song-timeline frame. Rewriting an
    // overlapping tail is intentional: track/key changes replace only audio
    // that has not reached the DirectSound play cursor yet.
    void push_timed(const std::int16_t* samples, std::size_t frames,
            std::uint64_t timeline_start_frame) {
        if (!samples || frames == 0) return;
        std::lock_guard lock(mutex_);
        if (!format_.valid() || capacity_frames_ == 0 || buffer_.empty()) {
            return;
        }

        if (frames >= capacity_frames_) {
            const auto dropped = frames - capacity_frames_;
            samples += dropped * static_cast<std::size_t>(format_.channels);
            timeline_start_frame += dropped;
            frames = capacity_frames_;
            read_frame_ = 0;
            write_frame_ = 0;
            available_frames_ = 0;
            timeline_first_frame_ = timeline_start_frame;
            timeline_valid_ = true;
        } else if (!timeline_valid_ || available_frames_ == 0) {
            read_frame_ = 0;
            write_frame_ = 0;
            available_frames_ = 0;
            timeline_first_frame_ = timeline_start_frame;
            timeline_valid_ = true;
        } else {
            const auto timeline_end =
                timeline_first_frame_ + available_frames_;
            if (timeline_start_frame < timeline_first_frame_
                    || timeline_start_frame > timeline_end) {
                read_frame_ = 0;
                write_frame_ = 0;
                available_frames_ = 0;
                timeline_first_frame_ = timeline_start_frame;
            } else if (timeline_start_frame < timeline_end) {
                const auto keep = static_cast<std::size_t>(
                    timeline_start_frame - timeline_first_frame_);
                available_frames_ = keep;
                write_frame_ = (read_frame_ + keep) % capacity_frames_;
            }
        }

        append_locked(samples, frames);
    }

    // Non-destructive timeline lookup used by Cast. The caller can request
    // the exact PCM interval represented by the Stage video frame without
    // consuming future DirectSound-buffered samples.
    std::size_t read_at(std::uint64_t timeline_start_frame,
            std::int16_t* output, std::size_t max_frames) const {
        if (!output || max_frames == 0) return 0;
        std::lock_guard lock(mutex_);
        if (!format_.valid() || !timeline_valid_ || available_frames_ == 0) {
            return 0;
        }
        const auto timeline_end = timeline_first_frame_ + available_frames_;
        if (timeline_start_frame < timeline_first_frame_
                || timeline_start_frame >= timeline_end) {
            return 0;
        }
        const auto offset = static_cast<std::size_t>(
            timeline_start_frame - timeline_first_frame_);
        const auto frames = std::min<std::size_t>(
            max_frames, available_frames_ - offset);
        copy_from_locked((read_frame_ + offset) % capacity_frames_,
            output, frames);
        return frames;
    }

    std::size_t read(std::int16_t* output, std::size_t max_frames) {
        if (!output || max_frames == 0) return 0;
        std::lock_guard lock(mutex_);
        if (!format_.valid() || available_frames_ == 0) return 0;
        const auto frames = std::min(max_frames, available_frames_);
        copy_from_locked(read_frame_, output, frames);
        read_frame_ = (read_frame_ + frames) % capacity_frames_;
        available_frames_ -= frames;
        if (timeline_valid_) timeline_first_frame_ += frames;
        return frames;
    }

private:
    void append_locked(const std::int16_t* samples, std::size_t frames) {
        const auto channels = static_cast<std::size_t>(format_.channels);
        if (frames >= capacity_frames_) {
            samples += (frames - capacity_frames_) * channels;
            frames = capacity_frames_;
            read_frame_ = 0;
            write_frame_ = 0;
            available_frames_ = 0;
        }
        if (available_frames_ + frames > capacity_frames_) {
            const auto overflow = available_frames_ + frames - capacity_frames_;
            read_frame_ = (read_frame_ + overflow) % capacity_frames_;
            available_frames_ -= overflow;
            if (timeline_valid_) timeline_first_frame_ += overflow;
        }
        auto remaining = frames;
        while (remaining > 0) {
            const auto contiguous =
                std::min(remaining, capacity_frames_ - write_frame_);
            const auto dst = write_frame_ * channels;
            const auto src = (frames - remaining) * channels;
            std::copy_n(samples + src, contiguous * channels,
                buffer_.data() + dst);
            write_frame_ = (write_frame_ + contiguous) % capacity_frames_;
            available_frames_ += contiguous;
            remaining -= contiguous;
        }
    }

    void copy_from_locked(std::size_t source_frame,
            std::int16_t* output, std::size_t frames) const {
        const auto channels = static_cast<std::size_t>(format_.channels);
        auto remaining = frames;
        while (remaining > 0) {
            const auto contiguous =
                std::min(remaining, capacity_frames_ - source_frame);
            const auto src = source_frame * channels;
            const auto dst = (frames - remaining) * channels;
            std::copy_n(buffer_.data() + src, contiguous * channels,
                output + dst);
            source_frame = (source_frame + contiguous) % capacity_frames_;
            remaining -= contiguous;
        }
    }

    void reset_locked() {
        format_ = {};
        buffer_.clear();
        capacity_frames_ = 0;
        read_frame_ = 0;
        write_frame_ = 0;
        available_frames_ = 0;
        timeline_first_frame_ = 0;
        timeline_valid_ = false;
    }

    mutable std::mutex mutex_;
    ProcessedPcmFormat format_;
    std::vector<std::int16_t> buffer_;
    std::size_t capacity_frames_{};
    std::size_t read_frame_{};
    std::size_t write_frame_{};
    std::size_t available_frames_{};
    std::uint64_t timeline_first_frame_{};
    bool timeline_valid_{};
};

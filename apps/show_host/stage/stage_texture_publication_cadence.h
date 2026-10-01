#pragma once

#include <chrono>
#include <cstdint>

// Per-consumer latest-frame cadence. The physical Stage producer remains at
// 60 Hz; only the Flutter publication copy/callback is reduced while a
// physical display is active. Missed deadlines are skipped instead of being
// replayed so a slow consumer can never request catch-up work.
class StageTexturePublicationCadence {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    [[nodiscard]] bool should_publish(
            bool physical_stage_active,
            TimePoint now) noexcept {
        return should_publish(physical_stage_active, 30, now);
    }

    [[nodiscard]] bool should_publish(
            bool physical_stage_active,
            std::uint32_t preview_fps,
            TimePoint now) noexcept {
        if (!physical_stage_active) {
            reset();
            return true;
        }

        const auto interval = preview_interval(preview_fps);

        if (!throttled_ || interval != interval_) {
            throttled_ = true;
            interval_ = interval;
            next_due_ = now + interval_;
            return true;
        }
        if (now < next_due_) return false;

        auto following = next_due_ + interval_;
        if (following <= now) {
            following = now + interval_;
        }
        next_due_ = following;
        return true;
    }

    void reset() noexcept {
        throttled_ = false;
        next_due_ = {};
        interval_ = {};
    }

    [[nodiscard]] static constexpr std::chrono::nanoseconds
    dual_screen_preview_interval() noexcept {
        return kDualScreenPreviewInterval;
    }

    [[nodiscard]] static constexpr std::chrono::nanoseconds
    preview_interval(std::uint32_t preview_fps) noexcept {
        const auto fps = preview_fps == 0
            ? 1U : (preview_fps > 60U ? 60U : preview_fps);
        return std::chrono::nanoseconds{1000000000LL / fps};
    }

private:
    static constexpr std::chrono::nanoseconds kDualScreenPreviewInterval{
        1000000000LL / 30LL};

    bool throttled_{};
    TimePoint next_due_{};
    std::chrono::nanoseconds interval_{};
};

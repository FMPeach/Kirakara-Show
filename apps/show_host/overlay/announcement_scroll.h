#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

struct AnnouncementCopies {
    double rightmost_x{};
    double pitch{};
    std::size_t count{};
    [[nodiscard]] double x(std::size_t i) const noexcept {
        return rightmost_x - static_cast<double>(i) * pitch;
    }
};

// Presentation only: no media position, playback state or song identity.
class AnnouncementScroll {
public:
    static constexpr double speed = 62.0;
    static constexpr double gap = 96.0;

    void hide() noexcept { showing_ = false; }

    [[nodiscard]] AnnouncementCopies sample(std::uint64_t submission,
            double width, double text_width, double now) noexcept {
        if (!std::isfinite(now) || !std::isfinite(width)
                || !std::isfinite(text_width) || width <= 0 || text_width <= 0) {
            hide();
            return {};
        }
        if (!showing_ || submission != submission_ || width != width_
                || text_width != text_width_ || now < last_now_) {
            started_ = now;
        }
        showing_ = true;
        submission_ = submission;
        width_ = width;
        text_width_ = text_width;
        last_now_ = now;
        const auto distance = std::max(0.0, now - started_) * speed;
        const auto pitch = text_width + gap;
        const auto available = std::floor(distance / pitch) + 1;
        const auto visible_limit = std::ceil((width + text_width) / pitch) + 1;
        return {width - std::fmod(distance, pitch), pitch,
            static_cast<std::size_t>(std::min(available, visible_limit))};
    }

private:
    bool showing_{};
    std::uint64_t submission_{};
    double width_{}, text_width_{}, started_{}, last_now_{};
};

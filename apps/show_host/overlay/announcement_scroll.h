#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct AnnouncementScrollSample {
    double x{};
    bool valid{};
};

// Presentation only: no media position, playback state or song identity.
class AnnouncementScroll {
public:
    static constexpr double speed = 62.0;

    void hide() noexcept { showing_ = false; }

    [[nodiscard]] AnnouncementScrollSample sample(std::uint64_t submission,
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
        // A single line travels from just outside the right edge until its
        // tail leaves the left edge; only then may the next pass enter.
        const auto travel = width + text_width;
        return {width - std::fmod(distance, travel), true};
    }

private:
    bool showing_{};
    std::uint64_t submission_{};
    double width_{}, text_width_{}, started_{}, last_now_{};
};

#include "host_hud_controller.h"
#include <algorithm>
#include <cmath>

HostHudPresentation HostHudController::presentation() const noexcept {
    if (tail_triggered_) return {true, static_cast<float>(std::clamp(tail_remaining_ / 10, 0.0, 1.0))};
    if (takeover_remaining_ > 0) return {true, static_cast<float>(takeover_remaining_ / 3)};
    return {};
}
HostHudPresentation HostHudController::update(const HostHudClockInput& in) {
    std::lock_guard lock(mutex_);
    if (!std::isfinite(in.position) || !std::isfinite(in.duration)
            || !std::isfinite(in.monotonic_seconds)) return presentation();
    const auto remember = [&] {
        last_now_ = in.monotonic_seconds; last_position_ = in.position;
        last_duration_ = in.duration; was_advancing_ = in.advancing; initialized_ = true;
    };
    if (!in.active) {
        takeover_remaining_ = tail_remaining_ = 0; tail_triggered_ = false;
        discontinuity_ = in.discontinuity_revision;
        remember(); return {};
    }
    if (in.program_instance != instance_) {
        if (!in.taken_over) {
            // New media can take seconds to open. Keep the old card phase
            // until that output has committed the new program.
            last_now_ = in.monotonic_seconds; was_advancing_ = false;
            return presentation();
        }
        instance_ = in.program_instance;
        discontinuity_ = in.discontinuity_revision;
        takeover_remaining_ = 3; tail_triggered_ = false; tail_remaining_ = 0;
        if (in.duration > 0 && in.duration <= 10) {
            tail_triggered_ = true;
            tail_remaining_ = std::clamp(in.duration - in.position, 0.0, 10.0);
        }
        remember(); return presentation();
    }
    if (!initialized_ || discontinuity_ != in.discontinuity_revision) {
        discontinuity_ = in.discontinuity_revision;
        remember(); return presentation();
    }
    const auto elapsed = in.advancing && was_advancing_
        ? std::max(0.0, in.monotonic_seconds - last_now_) : 0.0;
    takeover_remaining_ = std::max(0.0, takeover_remaining_ - elapsed);
    if (tail_triggered_) {
        tail_remaining_ = std::max(0.0, tail_remaining_ - elapsed);
    } else if (in.advancing && was_advancing_ && last_duration_ > 0 && in.duration > 0
            && last_position_ < in.duration - 10 && in.position >= in.duration - 10
            && in.position >= last_position_) {
        tail_triggered_ = true;
        tail_remaining_ = std::clamp(in.duration - in.position, 0.0, 10.0);
    }
    remember(); return presentation();
}

#include "host_hud_controller.h"
#include <algorithm>
#include <cmath>

HostHudPresentation HostHudController::presentation() const noexcept {
    const auto progress = tail_triggered_
        ? static_cast<float>(std::clamp(tail_remaining_ / 10, 0.0, 1.0)) : 0.0F;
    return {tail_triggered_ || takeover_remaining_ > 0, progress,
        instance_, last_now_, true};
}
HostHudPresentation HostHudController::update(const HostHudClockInput& in) {
    std::lock_guard lock(mutex_);
    if (!std::isfinite(in.position) || !std::isfinite(in.duration)
            || !std::isfinite(in.monotonic_seconds)) {
        auto result = presentation();
        if (std::isfinite(in.monotonic_seconds)) result.monotonic_seconds = in.monotonic_seconds;
        return result;
    }
    const auto remember = [&] {
        last_now_ = in.monotonic_seconds; last_position_ = in.position;
        last_duration_ = in.duration; was_advancing_ = in.advancing; initialized_ = true;
    };
    if (!in.active) {
        takeover_remaining_ = tail_remaining_ = 0; tail_triggered_ = false;
        discontinuity_ = in.discontinuity_revision;
        remember(); return presentation();
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

HostHudAppearance HostHudTransition::evaluate(double now) {
    constexpr double fade_seconds = .150;
    auto elapsed = std::max(0.0, now - started_);
    if (phase_ == Phase::fade_out && elapsed + 1e-9 >= fade_seconds) {
        current_ = target_;
        phase_ = current_ == HostHudScene::hidden ? Phase::steady : Phase::fade_in;
        started_ += fade_seconds;
        from_opacity_ = 0;
        elapsed = std::max(0.0, now - started_);
    }
    if (phase_ == Phase::fade_in && elapsed + 1e-9 >= fade_seconds) phase_ = Phase::steady;
    if (phase_ == Phase::steady)
        return {current_, current_ == HostHudScene::hidden ? 0.0F : 1.0F, false};
    const auto t = static_cast<float>(std::clamp(elapsed / fade_seconds, 0.0, 1.0));
    return {current_, phase_ == Phase::fade_out ? from_opacity_ * (1 - t)
        : from_opacity_ + (1 - from_opacity_) * t, true};
}

HostHudAppearance HostHudTransition::update(const HostHudTransitionInput& in) {
    if (!std::isfinite(in.monotonic_seconds)) return evaluate(started_);
    const auto now = in.monotonic_seconds;
    const auto allowed = [&](HostHudScene scene) {
        return scene == HostHudScene::qr ? in.qr_available
            : scene == HostHudScene::queue ? in.queue_available : true;
    };
    const auto desired = allowed(in.desired) ? in.desired : HostHudScene::hidden;
    const bool new_program = in.program_instance != program_ && in.program_instance != 0;
    program_ = in.program_instance;
    auto view = evaluate(now);
    // Manual disable and loss of LAN must remove an ineligible old card now,
    // even if a cross-scene fade-out was in progress.
    if (!allowed(current_)) {
        current_ = target_ = HostHudScene::hidden;
        phase_ = Phase::steady;
        view = {};
    }
    if (!in.enabled) {
        current_ = target_ = desired;
        phase_ = Phase::steady;
        return evaluate(now);
    }
    if (desired != target_) {
        target_ = desired;
        started_ = now;
        if (current_ == HostHudScene::hidden) {
            current_ = desired;
            phase_ = Phase::fade_in;
            from_opacity_ = 0;
        } else if (current_ == desired) {
            phase_ = Phase::fade_in;
            from_opacity_ = view.opacity;
        } else {
            phase_ = Phase::fade_out;
            from_opacity_ = view.opacity;
        }
    } else if (new_program && desired == HostHudScene::queue && phase_ == Phase::steady) {
        phase_ = Phase::fade_in;
        started_ = now;
        from_opacity_ = 0;
    }
    return evaluate(now);
}

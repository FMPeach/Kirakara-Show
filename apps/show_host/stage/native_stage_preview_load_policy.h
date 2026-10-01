#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

// Process-local pressure controller for the optional Flutter observer. It
// never changes the Native Stage producer or physical-present cadence. Only a
// sustained cluster (at least two events) of missed 60 Hz host slots lowers
// Preview publication; recovery is deliberately much slower to avoid
// thermal/scheduler oscillation.
enum class NativeStagePreviewLoadLevel : std::uint8_t {
    nominal_30_fps,
    reduced_20_fps,
    minimum_15_fps,
};

class NativeStagePreviewLoadPolicy {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void observe(
            bool physical_stage_active,
            TimePoint now,
            std::uint64_t missed_slots) noexcept {
        if (!physical_stage_active) {
            reset();
            return;
        }
        if (!active_ || (last_observation_ != TimePoint{}
                && now < last_observation_)) {
            active_ = true;
            level_ = NativeStagePreviewLoadLevel::nominal_30_fps;
            assessment_started_ = now;
            stable_since_ = now;
            missed_slots_ = 0;
            miss_events_ = 0;
        }
        last_observation_ = now;

        if (missed_slots != 0) {
            missed_slots_ += std::min<std::uint64_t>(
                missed_slots, kMissedSlotsToDegrade);
            ++miss_events_;
            stable_since_ = now;
        }

        if (now - assessment_started_ >= kAssessmentInterval) {
            const bool sustained_pressure =
                (missed_slots_ >= kMissedSlotsToDegrade
                    && miss_events_ >= kSevereMissEventsToDegrade)
                || miss_events_ >= kMissEventsToDegrade;
            assessment_started_ = now;
            missed_slots_ = 0;
            miss_events_ = 0;
            if (sustained_pressure
                    && level_
                        != NativeStagePreviewLoadLevel::minimum_15_fps) {
                level_ = level_
                        == NativeStagePreviewLoadLevel::nominal_30_fps
                    ? NativeStagePreviewLoadLevel::reduced_20_fps
                    : NativeStagePreviewLoadLevel::minimum_15_fps;
                stable_since_ = now;
                return;
            }
        }

        if (level_ != NativeStagePreviewLoadLevel::nominal_30_fps
                && now - stable_since_ >= kStableRecoveryInterval) {
            level_ = level_
                    == NativeStagePreviewLoadLevel::minimum_15_fps
                ? NativeStagePreviewLoadLevel::reduced_20_fps
                : NativeStagePreviewLoadLevel::nominal_30_fps;
            assessment_started_ = now;
            stable_since_ = now;
            missed_slots_ = 0;
            miss_events_ = 0;
        }
    }

    void reset() noexcept {
        level_ = NativeStagePreviewLoadLevel::nominal_30_fps;
        active_ = false;
        assessment_started_ = {};
        stable_since_ = {};
        last_observation_ = {};
        missed_slots_ = 0;
        miss_events_ = 0;
    }

    [[nodiscard]] NativeStagePreviewLoadLevel level() const noexcept {
        return level_;
    }

    [[nodiscard]] std::uint32_t preview_fps() const noexcept {
        switch (level_) {
        case NativeStagePreviewLoadLevel::minimum_15_fps:
            return 15;
        case NativeStagePreviewLoadLevel::reduced_20_fps:
            return 20;
        case NativeStagePreviewLoadLevel::nominal_30_fps:
        default:
            return 30;
        }
    }

    [[nodiscard]] static constexpr std::chrono::seconds
    assessment_interval() noexcept {
        return kAssessmentInterval;
    }

    [[nodiscard]] static constexpr std::chrono::seconds
    stable_recovery_interval() noexcept {
        return kStableRecoveryInterval;
    }

private:
    static constexpr std::chrono::seconds kAssessmentInterval{2};
    static constexpr std::chrono::seconds kStableRecoveryInterval{12};
    static constexpr std::uint64_t kMissedSlotsToDegrade{6};
    static constexpr std::uint32_t kSevereMissEventsToDegrade{2};
    static constexpr std::uint32_t kMissEventsToDegrade{3};

    NativeStagePreviewLoadLevel level_{
        NativeStagePreviewLoadLevel::nominal_30_fps};
    bool active_{};
    TimePoint assessment_started_{};
    TimePoint stable_since_{};
    TimePoint last_observation_{};
    std::uint64_t missed_slots_{};
    std::uint32_t miss_events_{};
};

#pragma once

#include "../cast/cast_av_sync.h"

#include <optional>
#include <utility>

template<class FramePtr>
struct ProgramPicture {
    FramePtr frame;
    FramePtr fallback;
};

// The raw lookup still decides whether the current program has a picture.
// The projection only changes which already-decoded picture is presented.
template<class FramePtr, class Lookup>
[[nodiscard]] ProgramPicture<FramePtr> select_program_picture(
        double raw_time,
        double raw_sample_time,
        std::optional<double> projected_time,
        bool hold_video_tail,
        double timestamp_tolerance,
        const FramePtr& last_submitted,
        Lookup&& lookup) {
    FramePtr raw_frame = lookup(raw_sample_time);
    const auto matches = [](const auto& frame, double time) {
        return frame && video_frame_matches_program_time(
            frame->timestamp_seconds(), frame->duration_seconds(), time);
    };
    if (!raw_frame || (!hold_video_tail && !matches(raw_frame, raw_time))) {
        return ProgramPicture<FramePtr>{};
    }
    if (!projected_time) {
        return ProgramPicture<FramePtr>{std::move(raw_frame), {}};
    }

    // A lagging projection can legitimately pick an older frame than the
    // raw lookup. Compare with the last frame actually aired, not with raw.
    const bool continuous = last_submitted
        && last_submitted->generation() == raw_frame->generation()
        && matches(last_submitted, raw_time);
    auto fallback = continuous && raw_frame->timestamp_seconds()
            < last_submitted->timestamp_seconds()
        ? last_submitted : std::move(raw_frame);
    if (hold_video_tail || !std::isfinite(*projected_time)
            || *projected_time == raw_time) {
        return ProgramPicture<FramePtr>{std::move(fallback), {}};
    }

    auto projected_frame = lookup(*projected_time);
    if (!matches(projected_frame, raw_time)
            || !matches(projected_frame, *projected_time)
            || (continuous && projected_frame->timestamp_seconds()
                < last_submitted->timestamp_seconds())) {
        return ProgramPicture<FramePtr>{std::move(fallback), {}};
    }
    const auto pts = projected_frame->timestamp_seconds();
    const auto duration = projected_frame->duration_seconds();
    // The raw lookup may hold a frame across a PTS gap. Only swap to a
    // candidate whose real interval covers the projected time.
    if (!std::isfinite(duration) || duration <= 0.0
            || pts > *projected_time + timestamp_tolerance
            || *projected_time > pts + duration + timestamp_tolerance) {
        return ProgramPicture<FramePtr>{std::move(fallback), {}};
    }
    if (projected_frame == fallback) {
        return ProgramPicture<FramePtr>{std::move(projected_frame), {}};
    }
    return ProgramPicture<FramePtr>{std::move(projected_frame), std::move(fallback)};
}

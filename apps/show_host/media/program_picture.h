#pragma once

#include "../cast/cast_av_sync.h"

#include <utility>

template<class FramePtr>
struct ProgramPicture {
    FramePtr frame;
    FramePtr raw_fallback;
    double time_seconds{};
};

// The caller keeps the existing queue/transport gates on raw time. Within
// one decoder snapshot preserve the old selection first, so projection can
// neither hide a genuine miss nor turn a valid raw frame into a miss.
template<class Lookup>
[[nodiscard]] auto select_program_picture(
        double raw_time,
        double raw_sample_time,
        double projected_time,
        bool hold_video_tail,
        double timestamp_tolerance,
        Lookup&& lookup) {
    auto raw_frame = lookup(raw_sample_time);
    ProgramPicture<decltype(raw_frame)> picture{{}, {}, raw_time};
    const auto matches = [](const auto& frame, double time) {
        return frame && video_frame_matches_program_time(
            frame->timestamp_seconds(), frame->duration_seconds(), time);
    };
    if (!raw_frame || (!hold_video_tail && !matches(raw_frame, raw_time))) {
        return picture;
    }
    picture.frame = std::move(raw_frame);
    if (hold_video_tail || !std::isfinite(projected_time)
            || projected_time == raw_time) {
        return picture;
    }

    auto projected_frame = lookup(projected_time);
    if (!matches(projected_frame, raw_time)
            || !matches(projected_frame, projected_time)) {
        return picture;
    }
    const auto pts = projected_frame->timestamp_seconds();
    const auto duration = projected_frame->duration_seconds();
    // texture_for can return an older frame through a PTS gap. That is
    // intentional on the raw path, but not evidence to advance the picture.
    if (!std::isfinite(duration) || duration <= 0.0
            || pts > projected_time + timestamp_tolerance
            || projected_time > pts + duration + timestamp_tolerance) {
        return picture;
    }
    if (projected_frame != picture.frame) {
        picture.raw_fallback = std::exchange(
            picture.frame, std::move(projected_frame));
    }
    picture.time_seconds = projected_time;
    return picture;
}

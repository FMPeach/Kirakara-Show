#include "cast_video_buffer_state.h"

#include <algorithm>
#include <cmath>

namespace {

double finite_non_negative(double value) noexcept {
    return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

bool covers_declared_end(
        const CastVideoBufferSnapshot& buffer,
        double tolerance_seconds) noexcept {
    if (buffer.frame_count == 0) return false;
    const auto duration = finite_non_negative(
        buffer.source_duration_seconds);
    if (duration <= 0.0) return false;
    return finite_non_negative(buffer.end_seconds)
            + finite_non_negative(tolerance_seconds)
        >= duration;
}

}  // namespace

double cast_video_buffer_runway(
        const CastVideoBufferSnapshot& buffer,
        double position_seconds) noexcept {
    const auto position = finite_non_negative(position_seconds);
    return std::max(0.0,
        finite_non_negative(buffer.end_seconds) - position);
}

bool cast_video_buffer_contains(
        const CastVideoBufferSnapshot& buffer,
        double position_seconds,
        double tolerance_seconds) noexcept {
    if (buffer.frame_count == 0) return false;
    const auto position = finite_non_negative(position_seconds);
    const auto start = finite_non_negative(buffer.start_seconds);
    const auto end = finite_non_negative(buffer.end_seconds);
    const auto tolerance = finite_non_negative(tolerance_seconds);
    return start <= position + tolerance
        && end + tolerance >= position;
}

bool cast_video_buffer_below_low_water(
        const CastVideoBufferSnapshot& buffer,
        double position_seconds,
        const CastVideoBufferWatermarks& watermarks) noexcept {
    if (buffer.availability == CastVideoBufferAvailability::fatal_error) {
        return true;
    }
    // EOS is a terminal runway, not recoverable starvation. The final video
    // frame may legitimately end before the presentation duration (DASH audio
    // packet alignment is a common example). Keep that frame available while
    // the program clock closes the small tail instead of entering an
    // underflow state from which no additional video can ever arrive.
    if (buffer.availability
            == CastVideoBufferAvailability::end_of_stream) {
        return buffer.frame_count == 0;
    }
    // Source Reader can delay its transport EOS for a progressive HTTP
    // response even after decoded coverage reaches the declared presentation
    // end. That complete tail needs no refill and must not trip the normal
    // low-water gate just before the clock reaches the duration.
    if (covers_declared_end(
            buffer, watermarks.timestamp_tolerance_seconds)) {
        return false;
    }
    if (!cast_video_buffer_contains(
            buffer, position_seconds,
            watermarks.timestamp_tolerance_seconds)) {
        return true;
    }
    return cast_video_buffer_runway(buffer, position_seconds)
        <= finite_non_negative(watermarks.low_seconds);
}

bool cast_video_buffer_reached_high_water(
        const CastVideoBufferSnapshot& buffer,
        double position_seconds,
        const CastVideoBufferWatermarks& watermarks) noexcept {
    if (buffer.availability == CastVideoBufferAvailability::fatal_error) {
        return false;
    }
    // A decoder that has positively reached EOS cannot satisfy the normal
    // high-water target. One retained frame is enough to leave priming or
    // underflow and let the clock converge on the terminal duration.
    if (buffer.availability
            == CastVideoBufferAvailability::end_of_stream) {
        return buffer.frame_count > 0;
    }
    if (covers_declared_end(
            buffer, watermarks.timestamp_tolerance_seconds)) {
        return true;
    }
    if (!cast_video_buffer_contains(
            buffer, position_seconds,
            watermarks.timestamp_tolerance_seconds)) {
        return false;
    }
    if (buffer.availability != CastVideoBufferAvailability::ready
            && buffer.availability
                != CastVideoBufferAvailability::retryable_starvation) {
        return false;
    }
    return buffer.frame_count >= watermarks.minimum_start_frames
        && cast_video_buffer_runway(buffer, position_seconds)
            >= finite_non_negative(watermarks.high_seconds);
}

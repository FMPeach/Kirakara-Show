#pragma once

#include "../audio/audio_cursor_projection.h"

#include <optional>

struct NativeStageLyricSample {
    double program_seconds{};
    double lyric_seconds{};
    std::uint64_t timeline_revision{};
};

// A presentation-only adjustment on the existing Native Stage tick. Missing
// audio reads may stop the estimate, but must not undo ink already shown.
class NativeStageLyricPresentation {
public:
    void reset() noexcept {
        projection_.reset();
        committed_.reset();
        program_seconds_ = -1.0;
    }

    [[nodiscard]] std::optional<NativeStageLyricSample> sample(
            const AudioPositionSample& audio,
            double monotonic_seconds,
            double duration_seconds,
            double clock_offset_seconds,
            double program_seconds,
            std::uint64_t timeline_revision,
            bool steady_playback) noexcept {
        if (!steady_playback || !std::isfinite(program_seconds)
                || program_seconds < 0.0) {
            reset();
            return std::nullopt;
        }
        if (timeline_revision != timeline_revision_
                || program_seconds < program_seconds_
                || duration_seconds != duration_seconds_
                || clock_offset_seconds != clock_offset_seconds_) {
            reset();
        }
        timeline_revision_ = timeline_revision;
        program_seconds_ = program_seconds;
        duration_seconds_ = duration_seconds;
        clock_offset_seconds_ = clock_offset_seconds;
        const auto lyric_seconds = audio_picture_time(projection_, audio,
            monotonic_seconds, duration_seconds, clock_offset_seconds,
            program_seconds, timeline_revision, true);
        return NativeStageLyricSample{
            program_seconds, committed_.keep(lyric_seconds, program_seconds),
            timeline_revision};
    }

    // Advance the floor only when the sink accepted a complete Stage frame.
    // An estimate for a dropped frame was never visible.
    void commit(const NativeStageLyricSample& sample) noexcept {
        if (sample.timeline_revision == timeline_revision_
                && sample.program_seconds == program_seconds_) {
            committed_.commit(sample.lyric_seconds);
        }
    }

private:
    AudioCursorProjection projection_;
    PresentedLyricTime committed_;
    double program_seconds_{-1.0};
    double duration_seconds_{};
    double clock_offset_seconds_{};
    std::uint64_t timeline_revision_{};
};

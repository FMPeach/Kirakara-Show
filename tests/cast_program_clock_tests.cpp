#include "../apps/show_host/cast/cast_program_clock.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void expect_near(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) > 1e-9) {
        std::cerr << message << ": expected " << expected
                  << ", got " << actual << '\n';
        std::exit(1);
    }
}

CastVideoBufferSnapshot ready_buffer(
        double start, double end, std::size_t frames = 5) {
    CastVideoBufferSnapshot buffer;
    buffer.frame_count = frames;
    buffer.start_seconds = start;
    buffer.end_seconds = end;
    buffer.source_duration_seconds = 20.0;
    buffer.availability = CastVideoBufferAvailability::ready;
    return buffer;
}

}  // namespace

int main() {
    CastProgramClock clock;
    clock.reset(1.0, 100.0);
    expect(clock.state() == CastProgramClockState::idle,
        "reset clock is not idle");
    expect_near(clock.position_seconds(), 1.0,
        "reset clock position changed");

    CastProgramClockInput input;
    input.monotonic_seconds = 100.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.audio_position_seconds = 1.0;
    input.video_buffer = ready_buffer(0.98, 1.25, 16);
    auto update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "high-water audio clock did not start");
    expect(update.request_audio_seek && update.request_audio_play,
        "audio clock start did not request aligned resume");
    expect_near(update.audio_seek_position_seconds, 1.0,
        "audio resume seek used the wrong position");

    input.monotonic_seconds = 100.03;
    input.audio_position_seconds = 1.03;
    input.video_buffer = ready_buffer(1.00, 1.12);
    update = clock.update(input);
    expect_near(update.position_seconds, 1.03,
        "audio-backed clock did not follow audio");
    expect(!update.request_audio_seek && !update.request_audio_play,
        "steady playback repeated audio resume actions");

    input.monotonic_seconds = 100.05;
    input.audio_position_seconds = 1.05;
    input.video_buffer = ready_buffer(1.00, 1.04, 2);
    input.video_buffer.availability =
        CastVideoBufferAvailability::retryable_starvation;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::underflow,
        "exhausted progressive buffer did not enter underflow");
    expect(update.request_audio_pause,
        "video underflow did not pause the audio clock");
    expect_near(update.position_seconds, 1.03,
        "underflow advanced beyond the last covered program time");

    input.monotonic_seconds = 100.30;
    input.audio_position_seconds = 1.05;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::underflow,
        "low buffer escaped underflow without reaching high-water");
    expect(!update.request_audio_pause,
        "stable underflow repeated the pause action");
    expect_near(update.position_seconds, 1.03,
        "underflow clock did not remain frozen");

    input.video_buffer = ready_buffer(1.00, 1.25, 12);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "recovered buffer did not resume playback");
    expect(update.request_audio_seek && update.request_audio_play,
        "underflow recovery did not realign audio");
    expect_near(update.audio_seek_position_seconds, 1.03,
        "underflow recovery sought past the frozen time");

    // A stale pre-seek audio sample must not rewind the resumed program.
    input.monotonic_seconds = 100.31;
    input.audio_position_seconds = 1.01;
    update = clock.update(input);
    expect_near(update.position_seconds, 1.03,
        "stale audio cursor rewound the program clock");

    input.playback_requested = false;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::paused,
        "pause request did not pause the clock");
    expect(update.request_audio_pause,
        "pause request did not pause audio");

    // User pause/resume is not a timeline discontinuity. If the existing
    // decoder queue still has a safe runway at the frozen position, resume
    // immediately instead of imposing the cold-start high-water gate again.
    input.monotonic_seconds = 100.40;
    input.playback_requested = true;
    input.audio_position_seconds = 1.03;
    input.video_buffer = ready_buffer(1.00, 1.10, 4);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "covered pause resume unnecessarily re-entered priming");
    expect(update.request_audio_seek && update.request_audio_play,
        "covered pause resume did not restart the aligned audio clock");
    expect_near(update.audio_seek_position_seconds, 1.03,
        "covered pause resume changed the frozen program position");

    // If the paused frame is no longer covered, resume must retain the safe
    // priming path and wait for a fresh decoder runway.
    input.playback_requested = false;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::paused,
        "second pause request did not pause the clock");
    input.monotonic_seconds = 100.50;
    input.playback_requested = true;
    input.video_buffer = ready_buffer(0.50, 0.90, 12);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::priming,
        "uncovered pause resume bypassed decoder priming");
    expect(!update.request_audio_play,
        "uncovered pause resume restarted audio before video was ready");

    // With no independent audio, steady time advances only while the video
    // buffer stays above low-water and rebases after recovery.
    clock.discontinuity(5.0, 200.0);
    input = {};
    input.monotonic_seconds = 200.0;
    input.playback_requested = true;
    input.has_video = true;
    input.video_buffer = ready_buffer(4.98, 5.20, 8);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "video-only clock did not start at high-water");
    input.monotonic_seconds = 200.10;
    input.video_buffer = ready_buffer(5.05, 5.22, 8);
    update = clock.update(input);
    expect_near(update.position_seconds, 5.10,
        "video-only clock did not advance from monotonic time");

    input.monotonic_seconds = 200.20;
    input.video_buffer = ready_buffer(5.05, 5.11, 2);
    input.video_buffer.availability =
        CastVideoBufferAvailability::retryable_starvation;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::underflow,
        "video-only starvation did not freeze the clock");
    expect_near(update.position_seconds, 5.10,
        "video-only underflow advanced beyond covered time");
    input.monotonic_seconds = 201.00;
    input.video_buffer = ready_buffer(5.05, 5.30, 8);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "video-only recovery did not resume");
    input.monotonic_seconds = 201.10;
    input.video_buffer = ready_buffer(5.10, 5.35, 8);
    update = clock.update(input);
    expect_near(update.position_seconds, 5.20,
        "video-only recovery did not rebase monotonic time");

    // Video-master DASH playback advances from monotonic time, but the
    // separate audio stream still has to follow every video buffer gate.
    clock.discontinuity(6.0, 250.0);
    input = {};
    input.monotonic_seconds = 250.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.video_buffer = ready_buffer(5.98, 6.30, 12);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "video-master clock with slave audio did not start");
    expect(update.request_audio_seek && update.request_audio_play,
        "video-master start did not align slave audio");

    input.monotonic_seconds = 250.10;
    input.video_buffer = ready_buffer(6.00, 6.04, 2);
    input.video_buffer.availability =
        CastVideoBufferAvailability::retryable_starvation;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::underflow,
        "video-master starvation did not freeze the program clock");
    expect(update.request_audio_pause,
        "video-master starvation did not pause slave audio");

    input.monotonic_seconds = 250.20;
    input.video_buffer = ready_buffer(6.00, 6.35, 12);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "video-master buffer recovery did not resume");
    expect(update.request_audio_seek && update.request_audio_play,
        "video-master recovery did not realign slave audio");
    expect_near(update.audio_seek_position_seconds, 6.0,
        "video-master recovery moved slave audio past the frozen time");

    // A final short buffer is playable without reaching the normal frame
    // count, then stops exactly at the media duration.
    clock.discontinuity(9.95, 300.0);
    input = {};
    input.monotonic_seconds = 300.0;
    input.playback_requested = true;
    input.has_video = true;
    input.video_buffer.frame_count = 1;
    input.video_buffer.start_seconds = 9.94;
    input.video_buffer.end_seconds = 10.0;
    input.video_buffer.source_duration_seconds = 10.0;
    input.video_buffer.availability =
        CastVideoBufferAvailability::end_of_stream;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "short EOS tail was not allowed to start");
    input.monotonic_seconds = 300.06;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::ended,
        "EOS clock did not enter ended state");
    expect_near(update.position_seconds, 10.0,
        "EOS clock did not clamp to duration");

    // DASH tracks commonly differ by one or two AAC packets. Once the video
    // decoder has positively reached EOS, exhausting the last frame before
    // the declared presentation duration must not bounce the clock into an
    // unrecoverable underflow/playing loop.
    clock.discontinuity(19.95, 320.0);
    input = {};
    input.monotonic_seconds = 320.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.video_buffer.frame_count = 1;
    input.video_buffer.start_seconds = 19.94;
    input.video_buffer.end_seconds = 20.0;
    input.video_buffer.source_duration_seconds = 20.08;
    input.video_buffer.availability =
        CastVideoBufferAvailability::end_of_stream;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "offset EOS tail was not allowed to start");
    input.monotonic_seconds = 320.06;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "exhausted EOS tail was misclassified as video underflow");
    expect(!update.request_audio_pause,
        "exhausted EOS tail paused slave audio before program end");
    input.monotonic_seconds = 320.14;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::ended,
        "offset EOS tail did not reach the declared program end");
    expect_near(update.position_seconds, 20.08,
        "offset EOS tail did not clamp to the declared duration");
    expect(update.request_audio_pause,
        "offset EOS tail did not stop slave audio at program end");

    // A progressive HTTP response can expose the complete presentation and
    // its declared duration before Source Reader reports EOS. Video-master
    // playback must still finish at that authoritative duration instead of
    // falling into permanent underflow while waiting for transport closure.
    clock.discontinuity(29.8, 340.0);
    input = {};
    input.monotonic_seconds = 340.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.video_buffer = ready_buffer(29.8, 30.0, 12);
    input.video_buffer.source_duration_seconds = 30.0;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "declared-duration video did not start");
    input.monotonic_seconds = 340.18;
    input.video_buffer.availability =
        CastVideoBufferAvailability::retryable_starvation;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "complete declared tail was misclassified as low-water starvation");
    input.monotonic_seconds = 340.25;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::ended,
        "declared-duration video waited indefinitely for transport EOS");
    expect_near(update.position_seconds, 30.0,
        "declared-duration video did not clamp to its presentation end");
    expect(update.request_audio_pause,
        "declared-duration video did not stop slave audio at program end");

    clock.discontinuity(2.0, 400.0);
    input = {};
    input.monotonic_seconds = 400.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.audio_position_seconds = 2.0;
    input.video_buffer.availability =
        CastVideoBufferAvailability::fatal_error;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::fatal_error,
        "fatal decoder state did not stop the program clock");
    expect(update.request_audio_pause,
        "fatal decoder state did not request audio pause");

    // Low-frame-rate visualizers can expose one long-duration frame that
    // covers far more time than several 60 fps frames. Readiness is based on
    // timestamp runway, not an arbitrary frame count.
    clock.discontinuity(7.0, 450.0);
    input = {};
    input.monotonic_seconds = 450.0;
    input.playback_requested = true;
    input.has_video = true;
    input.video_buffer = ready_buffer(6.9, 7.3, 1);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "long-duration video frame did not satisfy the time watermark");

    // Audio-only DASH content does not wait on a nonexistent video buffer.
    clock.discontinuity(3.0, 500.0);
    input = {};
    input.monotonic_seconds = 500.0;
    input.playback_requested = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.audio_position_seconds = 3.0;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "audio-only clock incorrectly waited for video");
    expect(update.request_audio_seek && update.request_audio_play,
        "audio-only clock did not start its base clock");

    // A catalog song is audio-master. Its background video may continue past
    // the separate vocal/accompaniment file, but natural audio EOF still ends
    // the program exactly once instead of oscillating through video underflow.
    clock.discontinuity(9.9, 600.0);
    input = {};
    input.monotonic_seconds = 600.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.audio_position_seconds = 9.9;
    input.end_on_audio_eof = true;
    input.video_buffer = ready_buffer(9.8, 10.4, 24);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "audio-master program did not start before audio EOF");
    input.monotonic_seconds = 600.1;
    input.audio_position_seconds = 10.0;
    input.audio_duration_seconds = 10.0;
    input.audio_end_of_stream = true;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::ended,
        "audio-master program ignored natural audio EOF");
    expect_near(update.position_seconds, 10.0,
        "audio-master EOF did not clamp to the audio duration");
    expect(update.request_audio_pause,
        "audio-master EOF did not close the audio clock");

    // The inverse duration mismatch is also audio-mastered: once a shorter
    // background video reaches EOS, its last frame is held while the audio
    // clock and lyric timeline continue to advance.
    clock.discontinuity(4.9, 650.0);
    input = {};
    input.monotonic_seconds = 650.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.audio_position_seconds = 4.9;
    input.audio_duration_seconds = 8.0;
    input.end_on_audio_eof = true;
    input.video_buffer.frame_count = 1;
    input.video_buffer.start_seconds = 4.9;
    input.video_buffer.end_seconds = 5.0;
    input.video_buffer.source_duration_seconds = 5.0;
    input.video_buffer.availability =
        CastVideoBufferAvailability::end_of_stream;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "short video ended an audio-master program");
    input.monotonic_seconds = 651.0;
    input.audio_position_seconds = 5.9;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "short video tail entered underflow while audio continued");
    expect_near(update.position_seconds, 5.9,
        "audio-master clock froze at the shorter video duration");
    expect(!update.request_audio_pause,
        "short video tail paused the audio-master clock");
    input.monotonic_seconds = 653.1;
    input.audio_position_seconds = 8.0;
    input.audio_end_of_stream = true;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::ended,
        "short-video program did not end at audio EOF");
    expect_near(update.position_seconds, 8.0,
        "short-video program did not finish on audio duration");

    // External/video-master playback uses the monotonic program clock while
    // its separate DASH audio remains a controlled slave. An audio stream
    // ending first must neither end nor strand the video timeline.
    clock.discontinuity(9.9, 700.0);
    input = {};
    input.monotonic_seconds = 700.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.audio_position_seconds = 9.9;
    input.audio_duration_seconds = 10.0;
    input.audio_end_of_stream = true;
    input.end_on_audio_eof = false;
    input.video_buffer = ready_buffer(9.8, 10.4, 24);
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "video-master program incorrectly ended on audio EOF");
    expect(update.request_audio_seek && update.request_audio_play,
        "video-master start did not align its slave audio stream");

    input.monotonic_seconds = 700.2;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::playing,
        "video-master program stopped after auxiliary audio EOF");
    expect(update.position_seconds > 10.05,
        "video-master clock did not advance independently of ended audio");

    input.monotonic_seconds = 700.5;
    input.video_buffer.end_seconds = 10.4;
    input.video_buffer.source_duration_seconds = 10.4;
    input.video_buffer.availability =
        CastVideoBufferAvailability::end_of_stream;
    update = clock.update(input);
    expect(update.state == CastProgramClockState::ended,
        "video-master program did not finish on video EOS");
    expect_near(update.position_seconds, 10.4,
        "video-master program finished at the wrong video position");
    expect(update.request_audio_pause,
        "video-master EOS did not stop its slave audio stream");

    // Host load commands and the Cast frame worker intentionally access this
    // small state machine from different threads. Exercise that contract
    // without involving decoder or renderer work.
    CastProgramClock concurrent_clock;
    std::thread controller([&] {
        for (int i = 0; i < 2000; ++i) {
            concurrent_clock.discontinuity(
                static_cast<double>(i % 7), static_cast<double>(i));
            concurrent_clock.pause_at(
                static_cast<double>(i % 7), static_cast<double>(i));
        }
    });
    CastProgramClockInput concurrent_input;
    concurrent_input.playback_requested = true;
    for (int i = 0; i < 2000; ++i) {
        concurrent_input.monotonic_seconds = static_cast<double>(i);
        const auto concurrent_update = concurrent_clock.update(concurrent_input);
        expect(std::isfinite(concurrent_update.position_seconds),
            "concurrent clock access produced an invalid position");
        static_cast<void>(concurrent_clock.state());
    }
    controller.join();

    return 0;
}

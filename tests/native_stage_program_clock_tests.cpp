#include "../apps/show_host/stage/native_stage_program_clock.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect_near(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) <= 1e-6) return;
    std::cerr << "FAIL: " << message << " (actual=" << actual
              << ", expected=" << expected << ")\n";
    std::exit(1);
}

NativeStageVideoBufferSnapshot ready_buffer(
        double start, double end, std::size_t frames = 8) {
    NativeStageVideoBufferSnapshot buffer;
    buffer.frame_count = frames;
    buffer.start_seconds = start;
    buffer.end_seconds = end;
    buffer.availability = MfD3D11VideoAvailability::ready;
    return buffer;
}

}  // namespace

int main() {
    MfD3D11VideoBufferStatus sparse_status;
    sparse_status.frame_count = 5;
    sparse_status.buffered_start_seconds = 0.0;
    sparse_status.buffered_end_seconds = 0.083;
    sparse_status.queued_end_seconds = 0.583;
    sparse_status.availability = MfD3D11VideoAvailability::ready;
    const auto sparse_snapshot = native_stage_buffer_snapshot(sparse_status);
    expect_near(sparse_snapshot.end_seconds, 0.583,
        "Native Stage did not bridge an already-decoded sparse PTS gap");

    NativeStageProgramClock clock;
    NativeStageProgramClockInput input;
    input.monotonic_seconds = 100.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.end_on_audio_eof = true;
    input.audio_position_seconds = 1.0;
    input.audio_duration_seconds = 8.0;
    input.video_buffer = ready_buffer(0.98, 1.30);
    clock.discontinuity(1.0, input.monotonic_seconds);

    auto update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "audio-master clock did not leave priming");
    expect(update.request_audio_seek && update.request_audio_play,
        "audio-master start did not align audio");
    expect_near(update.position_seconds, 1.0,
        "cold start did not preserve its requested origin");

    input.monotonic_seconds = 100.1;
    input.audio_position_seconds = 1.1;
    input.video_buffer = ready_buffer(1.05, 1.12, 2);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "low runway incorrectly entered buffering");
    expect_near(update.position_seconds, 1.1,
        "audio-master clock did not follow audio");

    input.monotonic_seconds = 100.2;
    input.audio_position_seconds = 1.2;
    input.video_buffer = ready_buffer(1.05, 1.12, 2);
    input.video_buffer.read_in_flight = true;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::buffering,
        "uncovered in-flight read did not enter buffering");
    expect(update.request_audio_pause,
        "buffering did not pause the audio clock");
    expect_near(update.position_seconds, 1.1,
        "buffering advanced beyond the last covered position");

    input.monotonic_seconds = 100.3;
    input.audio_position_seconds = 1.2;
    input.video_buffer = ready_buffer(1.08, 1.16, 3);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::buffering,
        "underflow recovered without a stable runway");

    input.monotonic_seconds = 100.4;
    input.video_buffer = ready_buffer(1.08, 1.24, 6);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "underflow did not recover after reaching the high watermark");
    expect(update.request_audio_seek && update.request_audio_play,
        "underflow recovery did not restart aligned audio");
    expect_near(update.audio_seek_position_seconds, 1.1,
        "underflow recovery moved the frozen position");

    // A 120 fps source fills the bounded five-frame Native queue before it
    // spans 100 ms. Reaching that bounded capacity must still release the
    // clock instead of leaving high-frame-rate media in priming forever.
    clock.discontinuity(2.0, 150.0);
    input.monotonic_seconds = 150.0;
    input.audio_position_seconds = 2.0;
    input.audio_end_of_stream = false;
    input.video_buffer = ready_buffer(1.995, 2.04, 5);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "full high-frame-rate queue did not release priming");

    // A promoted standby queue can still contain its full bounded frame
    // count even when every frame is already at (or behind) the frozen clock.
    // Frame count alone must not trigger a seek/play loop with no forward
    // coverage; leave the decoder running until a genuinely future frame
    // arrives.
    clock.discontinuity(2.081, 160.0);
    input.monotonic_seconds = 160.0;
    input.audio_position_seconds = 2.081;
    input.video_buffer = ready_buffer(2.0, 2.083, 5);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::priming,
        "stale full queue incorrectly released priming");
    expect(!update.request_audio_play,
        "stale full queue incorrectly started audio");
    input.monotonic_seconds = 160.1;
    input.video_buffer = ready_buffer(2.0, 2.125, 5);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "full queue with forward coverage did not release priming");

    // Audio-master media keeps the final video texture but continues lyrics
    // and program time until the authoritative audio EOF.
    clock.discontinuity(4.9, 200.0);
    input = {};
    input.monotonic_seconds = 200.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.end_on_audio_eof = true;
    input.audio_position_seconds = 4.9;
    input.audio_duration_seconds = 8.0;
    input.video_buffer.frame_count = 1;
    input.video_buffer.start_seconds = 4.9;
    input.video_buffer.end_seconds = 5.0;
    input.video_buffer.source_duration_seconds = 5.0;
    input.video_buffer.availability =
        MfD3D11VideoAvailability::end_of_stream;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "short video did not start in audio-master mode");
    input.monotonic_seconds = 201.0;
    input.audio_position_seconds = 5.9;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "short video tail incorrectly entered buffering");
    expect_near(update.position_seconds, 5.9,
        "short video froze the audio-master timeline");
    input.monotonic_seconds = 203.1;
    input.audio_position_seconds = 8.0;
    input.audio_end_of_stream = true;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::ended,
        "audio-master program ignored audio EOF");
    expect_near(update.position_seconds, 8.0,
        "audio-master program ended at the wrong duration");

    // Video-master media ignores auxiliary audio EOF and ends on video.
    clock.discontinuity(9.9, 300.0);
    input = {};
    input.monotonic_seconds = 300.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.video_buffer = ready_buffer(9.85, 10.30, 12);
    input.video_buffer.source_duration_seconds = 10.4;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "video-master program did not start");
    input.monotonic_seconds = 300.2;
    input.audio_end_of_stream = true;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "video-master program ended on auxiliary audio EOF");
    expect(update.position_seconds > 10.05,
        "video-master monotonic clock did not advance");
    input.monotonic_seconds = 300.6;
    input.video_buffer.end_seconds = 10.4;
    input.video_buffer.availability =
        MfD3D11VideoAvailability::end_of_stream;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::ended,
        "video-master program did not end at video duration");
    expect_near(update.position_seconds, 10.4,
        "video-master program ended at the wrong position");

    // A covered user pause resumes directly rather than entering cold-start
    // priming; this protects pause/play and replay behavior exposed to App.
    clock.discontinuity(2.0, 400.0);
    input = {};
    input.monotonic_seconds = 400.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.has_audio_clock = true;
    input.end_on_audio_eof = true;
    input.audio_position_seconds = 2.0;
    input.video_buffer = ready_buffer(1.98, 2.30, 8);
    update = clock.update(input);
    input.playback_requested = false;
    input.monotonic_seconds = 400.1;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::paused,
        "user pause did not enter paused state");
    input.playback_requested = true;
    input.monotonic_seconds = 401.0;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "covered pause resume unnecessarily re-entered priming");
    expect(update.request_audio_seek && update.request_audio_play,
        "covered pause resume did not restart audio");

    std::cout << "native stage program clock tests passed\n";
    return 0;
}


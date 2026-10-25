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
    // HTTP byte waits may target future read-ahead. They only stop playback
    // once current coverage runs out, never an already-ready start/recovery.
    {
        NativeStageProgramClock byte_wait_clock;
        NativeStageProgramClockInput byte_wait;
        byte_wait.playback_requested = true;
        byte_wait.has_video = true;
        byte_wait.has_audio_clock = true;
        byte_wait.has_audio_stream = true;
        byte_wait.end_on_audio_eof = true;
        byte_wait.video_buffer = ready_buffer(0.0, 0.3, 8);
        byte_wait.video_buffer.source_buffering_state =
            MfD3D11VideoSourceBufferingState::waiting_for_bytes;
        auto action = byte_wait_clock.update(byte_wait);
        expect(action.state == NativeStageProgramClockState::playing
                && action.request_audio_play,
            "future HTTP read-ahead blocked already-ready startup frames");
        byte_wait.audio_position_seconds = 0.5;
        byte_wait.monotonic_seconds = 0.5;
        action = byte_wait_clock.update(byte_wait);
        expect(action.state == NativeStageProgramClockState::buffering
                && action.request_audio_pause,
            "an actual byte wait with exhausted frames did not stop audio");
        byte_wait.video_buffer = ready_buffer(0.0, 0.3, 8);
        byte_wait.video_buffer.source_buffering_state =
            MfD3D11VideoSourceBufferingState::waiting_for_bytes;
        action = byte_wait_clock.update(byte_wait);
        expect(action.state == NativeStageProgramClockState::playing
                && action.request_audio_play,
            "future HTTP read-ahead blocked recovered frames");
    }
    MfD3D11VideoBufferStatus sparse_status;
    sparse_status.frame_count = 5;
    sparse_status.buffered_start_seconds = 0.0;
    sparse_status.buffered_end_seconds = 0.083;
    sparse_status.queued_end_seconds = 0.583;
    sparse_status.availability = MfD3D11VideoAvailability::ready;
    sparse_status.source_buffering_state =
        MfD3D11VideoSourceBufferingState::not_buffering;
    const auto sparse_snapshot = native_stage_buffer_snapshot(sparse_status);
    expect_near(sparse_snapshot.end_seconds, 0.583,
        "Native Stage did not bridge an already-decoded sparse PTS gap");
    expect(sparse_snapshot.source_buffering_state
            == MfD3D11VideoSourceBufferingState::not_buffering,
        "Native Stage lost the Media Foundation buffering state");

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
    input.video_buffer.source_buffering_state =
        MfD3D11VideoSourceBufferingState::not_buffering;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "complete media entered buffering because decode was late");
    expect(!update.request_audio_pause,
        "complete media paused audio because decode was late");
    expect_near(update.position_seconds, 1.2,
        "complete media stopped the audio-master clock");

    input.monotonic_seconds = 100.25;
    input.audio_position_seconds = 1.25;
    input.video_buffer.source_buffering_state =
        MfD3D11VideoSourceBufferingState::unknown;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "an uncovered queue without a source event entered buffering");
    expect_near(update.position_seconds, 1.25,
        "the clock stopped without a source buffering event");

    input.monotonic_seconds = 100.275;
    input.audio_position_seconds = 1.275;
    input.video_buffer.availability = MfD3D11VideoAvailability::opening;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "ordinary source reopen was misclassified as media buffering");

    input.monotonic_seconds = 100.3;
    input.audio_position_seconds = 1.3;
    input.video_buffer.availability = MfD3D11VideoAvailability::ready;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "late decode was misclassified as media buffering");
    expect(!update.request_audio_pause,
        "late decode paused the audio clock");

    input.monotonic_seconds = 100.45;
    input.audio_position_seconds = 1.45;
    input.video_buffer.source_buffering_state =
        MfD3D11VideoSourceBufferingState::buffering;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::buffering,
        "MEBufferingStarted did not enter buffering");
    expect(update.request_audio_pause,
        "buffering did not pause the audio clock");
    expect_near(update.position_seconds, 1.3,
        "buffering advanced beyond the last covered position");

    input.monotonic_seconds = 100.475;
    input.video_buffer = ready_buffer(1.08, 1.60, 8);
    input.video_buffer.source_buffering_state =
        MfD3D11VideoSourceBufferingState::buffering;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::buffering,
        "queue runway resumed before MEBufferingStopped");
    expect(!update.request_audio_play,
        "active source buffering restarted audio");

    // A user pause while the source is already buffering is still a user
    // pause. Resume must remain gated by video coverage instead of briefly
    // restarting audio and falling back into buffering again.
    input.playback_requested = false;
    input.monotonic_seconds = 100.5;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::paused,
        "user pause during buffering did not enter paused state");
    input.playback_requested = true;
    input.monotonic_seconds = 100.525;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::priming,
        "buffered pause resume bypassed video gating");
    expect(!update.request_audio_play,
        "buffered pause resume restarted audio without video coverage");

    input.monotonic_seconds = 100.55;
    input.audio_position_seconds = 1.45;
    input.video_buffer = ready_buffer(1.08, 1.16, 3);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::priming,
        "paused underflow recovered without a stable runway");

    input.monotonic_seconds = 100.65;
    input.video_buffer = ready_buffer(1.08, 1.40, 6);
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing,
        "underflow did not recover after reaching the high watermark");
    expect(update.request_audio_seek && update.request_audio_play,
        "underflow recovery did not restart aligned audio");
    expect_near(update.audio_seek_position_seconds, 1.3,
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

    // External DASH audio starts on a different worker/device from the
    // monotonic video clock. Startup delays and later device stalls must be
    // corrected during playback, not left until the next pause or byte wait.
    clock.discontinuity(20.0, 350.0);
    input = {};
    input.monotonic_seconds = 350.0;
    input.playback_requested = true;
    input.has_video = true;
    input.has_audio_stream = true;
    input.audio_position_seconds = 20.0;
    input.video_buffer = ready_buffer(19.98, 25.0, 120);
    input.video_buffer.source_duration_seconds = 30.0;
    update = clock.update(input);
    expect(update.request_audio_seek && update.request_audio_play,
        "external startup did not align audio");

    input.monotonic_seconds = 350.1;
    input.audio_position_seconds = 20.0;
    update = clock.update(input);
    expect(!update.request_audio_seek,
        "ordinary asynchronous audio startup triggered a corrective seek");

    input.monotonic_seconds = 350.6;
    input.audio_position_seconds = 20.1;
    update = clock.update(input);
    expect(update.request_audio_seek && update.request_audio_play,
        "delayed external audio was not realigned during playback");
    expect_near(update.audio_seek_position_seconds, 20.6,
        "delayed external audio did not seek to the video timeline");
    expect_near(update.position_seconds, 20.6,
        "audio correction changed the authoritative video timeline");

    input.monotonic_seconds = 350.7;
    update = clock.update(input);
    expect(!update.request_audio_seek,
        "external audio correction retried before the device could settle");

    input.monotonic_seconds = 351.2;
    input.audio_position_seconds = 21.65;
    update = clock.update(input);
    expect(update.request_audio_seek && update.request_audio_play,
        "external audio ahead of video was not corrected");
    expect_near(update.audio_seek_position_seconds, 21.2,
        "advanced external audio correction moved the video timeline");

    input.monotonic_seconds = 351.8;
    input.audio_position_seconds = 21.76;
    update = clock.update(input);
    expect(!update.request_audio_seek,
        "minor device cursor jitter caused an audible corrective seek");

    input.monotonic_seconds = 352.4;
    input.audio_position_seconds = 21.8;
    input.audio_end_of_stream = true;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::playing
            && !update.request_audio_seek && !update.request_audio_play,
        "short auxiliary audio was restarted after its natural EOF");

    input.monotonic_seconds = 353.0;
    input.audio_end_of_stream = false;
    input.video_buffer = {};
    input.video_buffer.source_buffering_state =
        MfD3D11VideoSourceBufferingState::waiting_for_bytes;
    update = clock.update(input);
    expect(update.state == NativeStageProgramClockState::buffering
            && update.request_audio_pause && !update.request_audio_seek,
        "drift correction bypassed external video buffering");

    input.monotonic_seconds = 353.5;
    input.video_buffer = ready_buffer(22.3, 26.0, 120);
    input.video_buffer.source_duration_seconds = 30.0;
    update = clock.update(input);
    expect(update.request_audio_seek && update.request_audio_play,
        "external buffer recovery did not realign audio");
    input.monotonic_seconds = 353.6;
    update = clock.update(input);
    expect(!update.request_audio_seek,
        "external recovery correction did not allow device startup to settle");

    // Catalog audio remains authoritative even when it advances at a
    // different pace from wall time.
    clock.discontinuity(20.0, 360.0);
    input.has_audio_clock = true;
    input.end_on_audio_eof = true;
    input.monotonic_seconds = 360.0;
    input.audio_position_seconds = 20.0;
    input.video_buffer = ready_buffer(19.98, 25.0, 120);
    update = clock.update(input);
    input.monotonic_seconds = 362.0;
    input.audio_position_seconds = 20.5;
    update = clock.update(input);
    expect(!update.request_audio_seek,
        "catalog audio was incorrectly corrected against wall time");
    expect_near(update.position_seconds, 20.5,
        "catalog playback lost its authoritative audio clock");

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

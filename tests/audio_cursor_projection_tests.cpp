#include "../apps/show_host/audio/audio_cursor_projection.h"
#include "../apps/show_host/cast/cast_program_clock.h"
#include "../apps/show_host/media/program_picture.h"
#include "../apps/show_host/stage/native_stage_program_clock.h"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Projection = AudioCursorProjection;
using Reading = AudioCursorReading;
constexpr auto max_offset_seconds = kMaxAudioPictureOffsetSeconds;
constexpr double epsilon = 1e-7;

void expect(bool ok, const char* message) {
    if (ok) return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect_near(double actual, double expected, const char* message) {
    if (std::abs(actual - expected) <= epsilon) return;
    std::cerr << "FAIL: " << message << " (actual=" << actual
              << ", expected=" << expected << ")\n";
    std::exit(1);
}

void test_quantized_reads() {
    for (double hz : {30.0, 60.0, 120.0, 144.0}) {
        for (int phase_ms = 0; phase_ms < 10; ++phase_ms) {
            Projection estimate;
            double wall = 0.0;
            auto last = estimate.observe({wall, 0.0, 0.0, 1, true});
            // Skip a presentation occasionally; time should advance by the
            // elapsed duration, not by one predetermined frame step.
            for (int i = 1; i <= 300; ++i) {
                const auto dt = (i % 31 == 0 ? 2.0 : 1.0) / hz
                    + (i % 2 ? 0.0003 : -0.0003);
                wall += dt;
                const auto raw =
                    std::floor((wall + phase_ms * 0.001) * 100.0) * 0.010;
                const auto position =
                    estimate.observe({wall, raw, 0.0, 1, true});
                expect(std::abs(position - raw) <=
                        max_offset_seconds + epsilon,
                    "projection exceeded the raw cursor bound");
                if (i > 3) {
                    expect_near(position - last, dt,
                        "projection retained a quantized step");
                }
                last = position;
            }
        }
    }
}

void test_long_run() {
    for (double rate : {0.9997, 1.0003}) {
        Projection estimate;
        double last = 0.0;
        for (int i = 0; i <= 60 * 600; ++i) {
            const auto wall = i / 60.0;
            const auto raw = std::floor(wall * rate * 100.0) * 0.010;
            const auto position = estimate.observe({wall, raw, 0.0, 1, true});
            expect(std::abs(position - raw) <= max_offset_seconds + epsilon,
                "long run drifted away from the device cursor");
            if (i > 2) {
                expect(std::abs(position - last - 1.0 / 60.0) < 0.001,
                    "slow device drift reintroduced visible cursor steps");
            }
            last = position;
        }
    }
}

void test_cache_and_stall() {
    Projection estimate;
    expect_near(estimate.observe({0.000, 3.0, 0.0, 1, true}), 3.0,
        "start must use the device cursor");
    expect_near(estimate.observe({0.005, 3.0, 0.0, 1, true}), 3.0,
        "projected before the cursor moved");
    expect_near(estimate.observe({0.015, 3.01, 0.0, 1, true, true}), 3.01,
        "cached progress should not enable projection");
    expect_near(estimate.observe({0.020, 3.02, 0.0, 1, true}), 3.02,
        "first direct movement should anchor exactly");
    expect_near(estimate.observe({0.030, 3.02, 0.0, 1, true, true}), 3.03,
        "short cache fallback did not use elapsed time");
    expect_near(estimate.observe({0.030, 3.02, 0.0, 1, true}), 3.03,
        "repeated read at the same time created progress");
    expect_near(estimate.observe({0.040, 3.03, 0.0, 1, true, true}), 3.04,
        "new worker-cached progress did not extend the bound");
    const auto limit = 3.03 + max_offset_seconds;
    for (int i = 5; i <= 100; ++i) {
        expect_near(estimate.observe({i * 0.01, 3.02, 0.0, 1, true, true}),
            limit, "an older cache entry rewound or extended the held cursor");
    }
    expect_near(estimate.observe({1.010, 10.0, 0.0, 1, true, true}), limit,
        "unexplained jump in cache was accepted");
    const auto recovered = estimate.observe({1.020, 3.04, 0.0, 1, true});
    expect(recovered > limit && recovered <= 3.04 + max_offset_seconds,
        "did not recover within bounds when device advanced");
    expect_near(estimate.observe({1.500, 3.52, 0.0, 1, true}), 3.52,
        "long gap reused an old presentation anchor");
    expect_near(estimate.observe({2.000, 0.5, 0.0, 1, true, true}), 3.52,
        "long gap re-anchored from an out-of-date cache entry");
}

void test_transport_boundaries() {
    Projection estimate;
    estimate.observe({0.00, 2.00, 0.0, 1, true});
    estimate.observe({0.02, 2.02, 0.0, 1, true});
    expect_near(estimate.observe({0.04, 2.03, 0.0, 1, true}), 2.04,
        "test setup did not reach a projected value");

    // Both a user pause and a buffering pause stop the device.
    expect_near(estimate.observe({0.05, 2.03, 0.0, 1, false}), 2.03,
        "pause carried projected time into the stopped cursor");
    expect_near(estimate.observe({2.00, 2.03, 0.0, 1, false}), 2.03,
        "paused cursor followed wall time");
    expect_near(estimate.observe({2.01, 2.03, 0.0, 1, true}), 2.03,
        "resume projected before actual progress");
    expect_near(estimate.observe({2.03, 2.04, 0.0, 1, true}), 2.04,
        "resume inherited its old estimate");
    expect_near(estimate.observe({2.04, 2.04, 0.0, 1, true}), 2.05,
        "resume did not re-enable projection");

    expect_near(estimate.observe({2.05, 2.045, 0.0, 2, true}), 2.045,
        "nearby seek must still re-anchor on a revision change");
    expect_near(estimate.observe({2.06, 0.0, 0.0, 3, true}), 0.0,
        "replay or next song inherited the old song position");
    expect_near(estimate.observe({2.08, 0.0, 0.0, 3, true}), 0.0,
        "first position after replay extrapolated a stopped cursor");
    expect_near(estimate.observe({2.10, 1.0, 0.0, 3, true}), 1.0,
        "unexpected direct jump was smoothed across");
    expect_near(estimate.observe({2.11, 0.8, 0.0, 3, true}), 0.8,
        "unexpected direct rewind was smoothed across");
    expect_near(estimate.observe({
            2.12, std::numeric_limits<double>::quiet_NaN(), 0.0, 3, true}),
        0.8, "invalid read added progress");
    expect_near(estimate.observe({2.13, 0.81, 0.0, 3, true}), 0.81,
        "valid read after error did not re-anchor");

    estimate.observe({3.00, 0.950, 1.0, 4, true});
    estimate.observe({3.01, 0.960, 1.0, 4, true});
    expect_near(estimate.observe({3.06, 0.980, 1.0, 4, true}),
        1.0 - max_offset_seconds, "projection entered the final frame");
    expect_near(estimate.observe({3.07, 0.990, 1.0, 4, true}), 0.990,
        "final cursor reads must be exact");
    expect_near(estimate.observe({3.09, 0.990, 1.0, 4, true, true}), 0.990,
        "a stale cursor manufactured EOF");
    expect_near(estimate.observe({3.13, 1.000, 1.0, 4, false}), 1.000,
        "final raw cursor did not reach EOF");
    expect_near(estimate.observe({3.14, 0.950, 0.95, 4, true}), 0.950,
        "shorter replacement track inherited an out-of-range position");
}

void test_picture_only() {
    Projection estimate;
    const auto project = [&](double wall, AudioPositionSample sample,
            double raw_program, bool steady = true, std::uint64_t revision = 1) {
        return audio_picture_time(estimate, sample, wall, 10.0, 0.25,
            raw_program, revision, steady);
    };
    project(0.00, {0.980, 1}, 1.230);
    project(0.01, {0.990, 1}, 1.240);
    expect_near(project(0.03, {1.000, 1}, 1.250), 1.260,
        "audio offset was not preserved in the picture time");
    expect_near(project(0.04, {1.000, 0, true}, 1.250), 1.250,
        "an untagged cache entry was used to project");
    expect_near(project(0.06, {1.010, 2}, 1.260), 1.260,
        "new running epoch inherited an old anchor");
    project(0.07, {1.020, 2}, 1.270);
    expect_near(project(0.09, {1.030, 2}, 1.280), 1.290,
        "a new running epoch did not recover");
    expect_near(project(0.10, {1.020, 2, true}, 1.280), 1.280,
        "stale audio overrode the program clock");
    expect_near(project(0.11, {1.040, 2}, 1.290, false), 1.290,
        "gate or transition advanced the picture");
    expect_near(project(0.12, {0.000, 2}, 0.250, true, 2), 0.250,
        "new program inherited the previous picture time");
}

struct TestFrame {
    double pts;
    double duration;
    [[nodiscard]] double timestamp_seconds() const { return pts; }
    [[nodiscard]] double duration_seconds() const { return duration; }
};

void test_picture_selection() {
    TestFrame previous{0.984, 0.016};
    TestFrame next{1.010, 0.016};
    constexpr double tolerance = 1.0 / 240.0;
    const auto available = [&](double time) {
        return time + tolerance >= next.pts ? &next : &previous;
    };
    const auto ready = select_program_picture(
        0.999, 0.999, 1.012, false, tolerance, available);
    expect(ready.frame == &next && ready.raw_fallback == &previous,
        "covered projection did not keep the original frame for fallback");
    expect_near(ready.time_seconds, 1.012, "picture did not use the selected time");

    const auto gap = select_program_picture(0.999, 0.999, 1.012, false,
        tolerance, [&](double) { return &previous; });
    expect(gap.frame == &previous && gap.raw_fallback == nullptr,
        "uncovered projection hid the valid raw frame");
    expect_near(gap.time_seconds, 0.999, "projection advanced through an uncovered gap");

    int lookups{};
    const auto miss = select_program_picture(0.999, 0.999, 1.012, false,
        tolerance, [&](double time) -> TestFrame* {
            ++lookups;
            return time < 1.0 ? nullptr : &next;
        });
    expect(miss.frame == nullptr && lookups == 1,
        "projection covered up a missing raw picture");
    const auto tail = select_program_picture(
        1.5, 1.0, 1.512, true, tolerance, available);
    expect(tail.frame == &previous, "projection interfered with a held video tail");
    expect_near(tail.time_seconds, 1.5, "held video tail changed its raw time");
}

void test_buffer_gates() {
    Projection projection;
    projection.observe({0.00, 0.980, 20.0, 1, true});
    projection.observe({0.01, 0.990, 20.0, 1, true});
    const auto picture = projection.observe({0.03, 1.000, 20.0, 1, true});
    expect_near(picture, 1.010, "test setup did not project beyond the raw cursor");

    CastProgramClock cast;
    cast.reset(0.95, 0.0);
    CastProgramClockInput c;
    c.has_audio_clock = c.has_audio_stream = c.has_video
        = c.playback_requested = c.end_on_audio_eof = true;
    c.audio_position_seconds = 0.95;
    c.video_buffer = {8, 0.94, 1.22, 20.0, CastVideoBufferAvailability::ready};
    expect(cast.update(c).request_audio_play, "Cast setup never started");
    c.video_buffer.end_seconds = 1.037;
    c.audio_position_seconds = 1.000;
    expect(cast_video_buffer_below_low_water(c.video_buffer, picture),
        "test projection did not cross Cast's low-water mark");
    const auto cast_playing = cast.update(c);
    expect(cast_playing.state == CastProgramClockState::playing
            && !cast_playing.request_audio_pause,
        "Cast paused while the raw cursor still had runway");
    c.audio_position_seconds = 1.010;
    const auto cast_wait = cast.update(c);
    expect(cast_wait.state == CastProgramClockState::underflow
            && cast_wait.request_audio_pause,
        "real Cast underflow did not pause audio");
    c.video_buffer.end_seconds = 1.220;
    const auto cast_resumed = cast.update(c);
    expect(cast_resumed.state == CastProgramClockState::playing
            && cast_resumed.request_audio_play && cast_resumed.request_audio_seek,
        "Cast did not recover at the raw cursor");

    NativeStageProgramClock stage;
    stage.reset(0.95, 0.0);
    NativeStageProgramClockInput s;
    s.has_audio_clock = s.has_audio_stream = s.has_video
        = s.playback_requested = s.end_on_audio_eof = true;
    s.audio_position_seconds = 0.95;
    s.video_buffer = {8, 0.94, 1.22, 20.0, MfD3D11VideoAvailability::ready};
    expect(stage.update(s).request_audio_play, "Stage setup never started");
    s.video_buffer.end_seconds = 1.003;
    s.video_buffer.source_buffering_state =
        MfD3D11VideoSourceBufferingState::waiting_for_bytes;
    s.audio_position_seconds = 1.000;
    const auto stage_playing = stage.update(s);
    expect(stage_playing.state == NativeStageProgramClockState::playing
            && !stage_playing.request_audio_pause,
        "Stage paused while the raw cursor was covered");
    s.audio_position_seconds = picture;
    const auto stage_wait = stage.update(s);
    expect(stage_wait.state == NativeStageProgramClockState::buffering
            && stage_wait.request_audio_pause,
        "a genuine byte wait did not pause Stage audio");
    s.video_buffer.end_seconds = 1.220;
    const auto stage_resumed = stage.update(s);
    expect(stage_resumed.state == NativeStageProgramClockState::playing
            && stage_resumed.request_audio_play && stage_resumed.request_audio_seek,
        "Stage did not recover at the raw cursor");
}

struct Steps {
    std::vector<double> errors_ms;
    int same_cursor{};
    int same_60fps_frame{};
    int skipped_60fps_frame{};

    void add(double prev, double current, double elapsed) {
        errors_ms.push_back(std::abs(current - prev - elapsed) * 1000.0);
        if (std::abs(current - prev) <= epsilon) ++same_cursor;
        // Offline CFR approximation of texture_for's PTS <= t + 1/240.
        const auto frame = [](double t) {
            return std::floor((t + 1.0 / 240.0) * 60.0 + epsilon);
        };
        if (frame(current) == frame(prev)) ++same_60fps_frame;
        if (frame(current) > frame(prev) + 1.0) ++skipped_60fps_frame;
    }

    void print(const char* label, bool cast) {
        expect(!errors_ms.empty(), "no adjacent samples in replay interval");
        std::sort(errors_ms.begin(), errors_ms.end());
        const auto p95 = static_cast<std::size_t>(
            std::ceil(errors_ms.size() * 0.95)) - 1;
        std::cout << label << ": same_cursor=" << same_cursor
                  << '/' << errors_ms.size()
                  << ", p95_step_error_ms=" << errors_ms[p95];
        if (cast) {
            std::cout << ", simulated_60fps_repeats=" << same_60fps_frame
                      << ", simulated_60fps_skips=" << skipped_60fps_frame;
        }
        std::cout << '\n';
    }
};

void replay(std::istream& input, bool cast, double from, double to,
        double duration) {
    std::string line;
    expect(static_cast<bool>(std::getline(input, line)), "missing CSV header");
    expect(line.starts_with(cast ? "slot,frame,wall,clock,audio,pts,runway,cached,"
                     : "tick,song,audio,video_pts,runway,"),
        "unrecognized cursor trace schema");
    Projection estimate;
    Steps before;
    Steps after;
    double prev_wall{};
    double prev_raw{};
    double prev_position{};
    double min_offset{};
    double max_offset{};
    bool prev_in_window{};
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::replace(line.begin(), line.end(), ',', ' ');
        std::istringstream row(line);
        Reading reading;
        reading.duration_seconds = duration;
        reading.device_running = true;
        double song{};
        double ignore{};
        int cached{};
        if (cast) {
            expect(static_cast<bool>(row >> ignore >> ignore
                >> reading.wall_seconds >> song >> reading.raw_seconds
                >> ignore >> ignore >> cached), "invalid Cast trace row");
        } else {
            int has_video{};
            int buffering{};
            expect(static_cast<bool>(row >> reading.wall_seconds >> song
                >> reading.raw_seconds >> ignore >> ignore >> ignore
                >> ignore >> ignore >> ignore >> cached >> has_video
                >> buffering), "invalid Stage trace row");
            reading.device_running = has_video && !buffering;
        }
        reading.from_cache = cached != 0;
        const auto position = estimate.observe(reading);
        const bool in_window = song >= from && song < to;
        if (in_window) {
            const auto offset = position - reading.raw_seconds;
            min_offset = std::min(min_offset, offset);
            max_offset = std::max(max_offset, offset);
            if (prev_in_window) {
                const auto elapsed = reading.wall_seconds - prev_wall;
                before.add(prev_raw, reading.raw_seconds, elapsed);
                after.add(prev_position, position, elapsed);
            }
        }
        prev_in_window = in_window;
        prev_wall = reading.wall_seconds;
        prev_raw = reading.raw_seconds;
        prev_position = position;
    }
    std::cout << std::fixed << std::setprecision(4);
    before.print("raw", cast);
    after.print("candidate", cast);
    std::cout << "candidate_minus_raw_ms=[" << min_offset * 1000.0
              << ", " << max_offset * 1000.0 << "]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 1) {
        expect(argc == 6, "usage: stage|cast trace.csv from to duration");
        const std::string_view kind(argv[1]);
        expect(kind == "stage" || kind == "cast", "unknown replay kind");
        std::ifstream input(argv[2]);
        expect(static_cast<bool>(input), "could not open trace");
        replay(input, kind == "cast", std::stod(argv[3]), std::stod(argv[4]),
            std::stod(argv[5]));
        return 0;
    }
    test_quantized_reads();
    test_long_run();
    test_cache_and_stall();
    test_transport_boundaries();
    test_picture_only();
    test_picture_selection();
    test_buffer_gates();
    std::cout << "PASS: bounded audio cursor projection\n";
}

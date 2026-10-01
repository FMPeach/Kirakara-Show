#include "../apps/show_host/show_host_api.h"
#include "../apps/show_host/cast/mpeg_ts_muxer.h"

#ifndef _WINSOCKAPI_
#include <winsock2.h>
#endif
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct PacketCounts {
    std::uint64_t video{};
    std::uint64_t audio{};
    std::uint64_t total{};
};

std::chrono::seconds parse_soak_duration(int argc, wchar_t** argv) {
    constexpr std::wstring_view prefix = L"--soak-seconds=";
    for (int index = 9; index < argc; ++index) {
        const std::wstring_view argument{argv[index]};
        if (!argument.starts_with(prefix)) continue;
        const auto value = argument.substr(prefix.size());
        if (value.empty()) return {};
        wchar_t* end{};
        const auto seconds = std::wcstoul(value.data(), &end, 10);
        if (end == value.data() || *end != L'\0') return {};
        return std::chrono::seconds{seconds};
    }
    return {};
}

std::uint64_t file_time_ticks(const FILETIME& value) noexcept {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    return ticks.QuadPart;
}

struct ProcessSnapshot {
    std::uint64_t cpu_ticks{};
    std::size_t working_set{};
    std::size_t private_bytes{};
    DWORD handles{};
};

ProcessSnapshot process_snapshot() noexcept {
    auto process = GetCurrentProcess();
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    static_cast<void>(GetProcessTimes(
        process, &creation, &exit, &kernel, &user));
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    static_cast<void>(GetProcessMemoryInfo(
        process,
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
        sizeof(memory)));
    DWORD handles{};
    static_cast<void>(GetProcessHandleCount(process, &handles));
    return ProcessSnapshot{
        file_time_ticks(kernel) + file_time_ticks(user),
        memory.WorkingSetSize,
        memory.PrivateUsage,
        handles,
    };
}

class TsClient {
public:
    ~TsClient() { stop(); }

    bool start(std::uint16_t port) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        winsock_started_ = true;
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (connect(socket_, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
            return false;
        }
        constexpr char request[] =
            "GET /cast/stage.ts HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n\r\n";
        if (send(socket_, request, static_cast<int>(sizeof(request) - 1), 0)
                == SOCKET_ERROR) {
            return false;
        }
        stop_.store(false);
        worker_ = std::thread([this] { read_loop(); });
        return true;
    }

    void stop() {
        stop_.store(true);
        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (worker_.joinable()) worker_.join();
        if (winsock_started_) {
            WSACleanup();
            winsock_started_ = false;
        }
    }

    PacketCounts counts() const noexcept {
        return PacketCounts{
            video_.load(),
            audio_.load(),
            total_.load(),
        };
    }

    std::uint64_t malformed_packets() const noexcept {
        return malformed_.load();
    }

    std::uint64_t pts_regressions() const noexcept {
        return pts_regressions_.load();
    }

    std::uint64_t max_video_pts_gap() const noexcept {
        return max_video_pts_gap_.load();
    }

    std::uint64_t max_audio_pts_gap() const noexcept {
        return max_audio_pts_gap_.load();
    }

    std::uint64_t max_emission_pts_lead() const noexcept {
        return max_emission_pts_lead_.load();
    }

    std::uint64_t max_nearest_av_pts_delta() const noexcept {
        if (video_pts_.empty() || audio_pts_.empty()) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        const auto overlap_start = std::max(
            video_pts_.front(), audio_pts_.front());
        const auto overlap_end = std::min(
            video_pts_.back(), audio_pts_.back());
        if (overlap_start > overlap_end) {
            return std::numeric_limits<std::uint64_t>::max();
        }

        const auto max_nearest = [overlap_start, overlap_end](
                const std::vector<std::uint64_t>& source,
                const std::vector<std::uint64_t>& target) {
            std::uint64_t maximum{};
            for (const auto value : source) {
                if (value < overlap_start || value > overlap_end) continue;
                const auto upper = std::lower_bound(
                    target.begin(), target.end(), value);
                auto nearest = std::numeric_limits<std::uint64_t>::max();
                if (upper != target.end()) nearest = *upper - value;
                if (upper != target.begin()) {
                    nearest = std::min(nearest, value - *std::prev(upper));
                }
                maximum = std::max(maximum, nearest);
            }
            return maximum;
        };
        return std::max(
            max_nearest(video_pts_, audio_pts_),
            max_nearest(audio_pts_, video_pts_));
    }

private:
    static std::uint64_t read_pts90k(const std::uint8_t* value) {
        return (static_cast<std::uint64_t>((value[0] >> 1U) & 0x07U) << 30U)
            | (static_cast<std::uint64_t>(value[1]) << 22U)
            | (static_cast<std::uint64_t>((value[2] >> 1U) & 0x7FU) << 15U)
            | (static_cast<std::uint64_t>(value[3]) << 7U)
            | static_cast<std::uint64_t>(value[4] >> 1U);
    }

    void observe_pts(std::uint16_t pid, const std::uint8_t* packet) {
        if ((packet[1] & 0x40U) == 0) return;
        const auto adaptation = (packet[3] >> 4U) & 0x03U;
        if (adaptation == 0 || adaptation == 2) return;
        std::size_t offset = 4;
        if (adaptation == 3) {
            offset += 1U + packet[4];
        }
        if (offset + 14U > MpegTsMuxer::kPacketSize
                || packet[offset] != 0 || packet[offset + 1] != 0
                || packet[offset + 2] != 1
                || (packet[offset + 7] & 0x80U) == 0) {
            return;
        }
        const auto pts = read_pts90k(packet + offset + 9U);
        auto& last = pid == MpegTsMuxer::kVideoPid
            ? last_video_pts_ : last_audio_pts_;
        auto& max_gap = pid == MpegTsMuxer::kVideoPid
            ? max_video_pts_gap_ : max_audio_pts_gap_;
        if (last != 0) {
            if (pts < last) {
                pts_regressions_.fetch_add(1);
            } else {
                const auto gap = pts - last;
                auto previous = max_gap.load();
                while (gap > previous
                        && !max_gap.compare_exchange_weak(previous, gap)) {}
            }
        }
        last = pts;
        auto& timeline = pid == MpegTsMuxer::kVideoPid
            ? video_pts_ : audio_pts_;
        if (timeline.empty() || timeline.back() != pts) {
            timeline.push_back(pts);
        }

        if (last_video_pts_ != 0 && last_audio_pts_ != 0) {
            const auto delta = last_video_pts_ > last_audio_pts_
                ? last_video_pts_ - last_audio_pts_
                : last_audio_pts_ - last_video_pts_;
            auto previous = max_emission_pts_lead_.load();
            while (delta > previous
                    && !max_emission_pts_lead_.compare_exchange_weak(
                        previous, delta)) {}
        }
    }

    void process_packets(std::vector<std::uint8_t>& pending) {
        while (pending.size() >= MpegTsMuxer::kPacketSize) {
            if (pending.front() != 0x47) {
                const auto next = std::find(
                    pending.begin() + 1, pending.end(), 0x47);
                if (next == pending.end()) {
                    pending.clear();
                    return;
                }
                pending.erase(pending.begin(), next);
                malformed_.fetch_add(1);
                continue;
            }
            const auto pid = static_cast<std::uint16_t>(
                ((pending[1] & 0x1FU) << 8U) | pending[2]);
            if (pid == MpegTsMuxer::kVideoPid
                    || pid == MpegTsMuxer::kAudioPid) {
                observe_pts(pid, pending.data());
            }
            if (pid == MpegTsMuxer::kVideoPid) video_.fetch_add(1);
            if (pid == MpegTsMuxer::kAudioPid) audio_.fetch_add(1);
            total_.fetch_add(1);
            pending.erase(pending.begin(),
                pending.begin() + MpegTsMuxer::kPacketSize);
        }
    }

    void read_loop() {
        std::vector<std::uint8_t> pending;
        bool header_complete{};
        std::string header;
        std::uint8_t buffer[64 * 1024]{};
        while (!stop_.load()) {
            const auto received = recv(socket_,
                reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
            if (received <= 0) break;
            if (!header_complete) {
                header.append(reinterpret_cast<char*>(buffer), received);
                const auto end = header.find("\r\n\r\n");
                if (end == std::string::npos) continue;
                expect(header.starts_with("HTTP/1.1 200"),
                    "Cast HTTP response");
                const auto payload_start = end + 4;
                pending.insert(pending.end(),
                    header.begin() + static_cast<std::ptrdiff_t>(payload_start),
                    header.end());
                header_complete = true;
            } else {
                pending.insert(pending.end(), buffer, buffer + received);
            }
            process_packets(pending);
        }
    }

    SOCKET socket_{INVALID_SOCKET};
    bool winsock_started_{};
    std::thread worker_;
    std::atomic<bool> stop_{true};
    std::atomic<std::uint64_t> video_{};
    std::atomic<std::uint64_t> audio_{};
    std::atomic<std::uint64_t> total_{};
    std::atomic<std::uint64_t> malformed_{};
    std::atomic<std::uint64_t> pts_regressions_{};
    std::atomic<std::uint64_t> max_video_pts_gap_{};
    std::atomic<std::uint64_t> max_audio_pts_gap_{};
    std::atomic<std::uint64_t> max_emission_pts_lead_{};
    std::uint64_t last_video_pts_{};
    std::uint64_t last_audio_pts_{};
    std::vector<std::uint64_t> video_pts_;
    std::vector<std::uint64_t> audio_pts_;
};

void expect_progress(const PacketCounts& before, const PacketCounts& after,
        std::string_view phase) {
    if (after.video <= before.video || after.audio <= before.audio) {
        std::cerr << "FAIL: " << phase << " did not advance both streams: "
                  << "video " << before.video << " -> " << after.video
                  << ", audio " << before.audio << " -> " << after.audio
                  << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void run_cast_soak(
        ShowHostHandle host,
        TsClient& client,
        const ShowHostCastCapabilityReport& capability,
        std::chrono::seconds duration) {
    if (duration.count() <= 0) return;

    ShowHostCastPipelineStats started_stats{};
    started_stats.struct_size = sizeof(started_stats);
    expect(show_host_get_cast_pipeline_stats(host, &started_stats),
        "read Cast statistics before soak");
    const auto started_packets = client.counts();
    const auto started_process = process_snapshot();
    const auto started_at = std::chrono::steady_clock::now();
    auto sample_at = started_at;
    auto sample_stats = started_stats;
    auto sample_packets = started_packets;
    double minimum_encoder_fps = std::numeric_limits<double>::max();
    double minimum_video_packet_rate = std::numeric_limits<double>::max();
    std::uint64_t replay_count{};

    std::cout << "CAST_SOAK_BEGIN seconds=" << duration.count()
              << " windows_build=" << capability.windows_build
              << " vendor_id=0x" << std::hex << capability.vendor_id
              << " device_id=0x" << capability.device_id
              << " adapter_luid=0x" << capability.adapter_luid
              << std::dec << " backend=" << capability.backend
              << " encoder_copy=" << capability.requires_encoder_copy
              << std::endl;

    const auto deadline = started_at + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(100ms);
        const auto now = std::chrono::steady_clock::now();
        const auto media_duration = show_host_get_duration(host);
        const auto position = show_host_get_position(host);
        if (media_duration > 1.0 && position >= media_duration - 0.5) {
            show_host_seek(host, 0.0);
            show_host_play(host);
            ++replay_count;
        }
        if (now - sample_at < 5s) continue;

        ShowHostCastPipelineStats current{};
        current.struct_size = sizeof(current);
        expect(show_host_get_cast_pipeline_stats(host, &current),
            "read periodic Cast statistics during soak");
        const auto packets = client.counts();
        const auto elapsed = std::chrono::duration<double>(now - sample_at)
            .count();
        const auto encoded_frames =
            current.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count
            - sample_stats.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count;
        const auto video_packets = packets.video - sample_packets.video;
        const auto encoder_fps = encoded_frames / elapsed;
        const auto video_packet_rate = video_packets / elapsed;
        minimum_encoder_fps = std::min(minimum_encoder_fps, encoder_fps);
        minimum_video_packet_rate = std::min(
            minimum_video_packet_rate, video_packet_rate);

        const auto qpc_ms = [&current](std::uint32_t event) {
            return current.qpc_frequency == 0 ? 0.0
                : 1000.0 * static_cast<double>(
                    current.events[event].maximum_gap_qpc)
                    / static_cast<double>(current.qpc_frequency);
        };
        std::cout << std::fixed << std::setprecision(3)
                  << "CAST_SOAK_WINDOW elapsed_s="
                  << std::chrono::duration<double>(now - started_at).count()
                  << " position_s=" << position
                  << " encoder_fps=" << encoder_fps
                  << " video_packets_s=" << video_packet_rate
                  << " missed_slots="
                  << current.missed_output_slots
                        - sample_stats.missed_output_slots
                  << " encoder_drops="
                  << current.encoder_drops - sample_stats.encoder_drops
                  << " underflows="
                  << current.decoder_underflows
                        - sample_stats.decoder_underflows
                  << " encoder_gap_max_ms="
                  << qpc_ms(SHOW_CAST_EVENT_ENCODER_OUTPUT)
                  << " http_gap_max_ms="
                  << qpc_ms(SHOW_CAST_EVENT_HTTP_SEND)
                  << " owners="
                  << show_host_get_active_video_decoder_owners(host)
                  << std::endl;

        expect(encoded_frames > 0,
            "Cast encoder emitted no frame in a five-second soak window");
        expect(video_packets > 0,
            "Cast receiver got no video packet in a five-second soak window");
        expect(show_host_get_active_video_decoder_owners(host) == 1,
            "Cast soak no longer has exactly one video decoder owner");
        expect(client.malformed_packets() == 0,
            "Cast soak receiver lost MPEG-TS packet alignment");
        expect(client.pts_regressions() == 0,
            "Cast soak receiver observed a PTS regression");
        sample_at = now;
        sample_stats = current;
        sample_packets = packets;
    }

    ShowHostCastPipelineStats finished_stats{};
    finished_stats.struct_size = sizeof(finished_stats);
    expect(show_host_get_cast_pipeline_stats(host, &finished_stats),
        "read Cast statistics after soak");
    const auto finished_packets = client.counts();
    const auto finished_process = process_snapshot();
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started_at).count();
    const auto encoded_frames =
        finished_stats.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count
        - started_stats.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count;
    const auto average_encoder_fps = encoded_frames / elapsed;
    const auto average_video_packet_rate =
        (finished_packets.video - started_packets.video) / elapsed;
    if (minimum_encoder_fps == std::numeric_limits<double>::max()) {
        minimum_encoder_fps = average_encoder_fps;
        minimum_video_packet_rate = average_video_packet_rate;
    }
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto process_seconds = static_cast<double>(
        finished_process.cpu_ticks - started_process.cpu_ticks) / 10000000.0;
    const auto one_core_cpu = process_seconds / elapsed * 100.0;
    const auto machine_cpu = one_core_cpu
        / static_cast<double>(std::max<DWORD>(1, system.dwNumberOfProcessors));
    const auto mib_delta = [](std::size_t after, std::size_t before) {
        return static_cast<double>(after) / (1024.0 * 1024.0)
            - static_cast<double>(before) / (1024.0 * 1024.0);
    };

    std::cout << std::fixed << std::setprecision(3)
              << "CAST_SOAK_END elapsed_s=" << elapsed
              << " encoder_fps=" << average_encoder_fps
              << " min_5s_encoder_fps=" << minimum_encoder_fps
              << " video_packets_s=" << average_video_packet_rate
              << " min_5s_video_packets_s=" << minimum_video_packet_rate
              << " missed_slots="
              << finished_stats.missed_output_slots
                    - started_stats.missed_output_slots
              << " encoder_drops="
              << finished_stats.encoder_drops - started_stats.encoder_drops
              << " underflows="
              << finished_stats.decoder_underflows
                    - started_stats.decoder_underflows
              << " recoveries="
              << finished_stats.decoder_recoveries
                    - started_stats.decoder_recoveries
              << " replays=" << replay_count
              << " cpu_one_core_pct=" << one_core_cpu
              << " cpu_machine_pct=" << machine_cpu
              << " working_set_delta_mib="
              << mib_delta(
                    finished_process.working_set,
                    started_process.working_set)
              << " private_delta_mib="
              << mib_delta(
                    finished_process.private_bytes,
                    started_process.private_bytes)
              << " handle_delta="
              << static_cast<std::int64_t>(finished_process.handles)
                    - static_cast<std::int64_t>(started_process.handles)
              << std::endl;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    expect(argc >= 9,
        "usage: smoke video1 krl1 vocal1 inst1 video2 krl2 vocal2 inst2 "
        "[--soak-seconds=N]");
    const auto soak_duration = parse_soak_duration(argc, argv);
    auto host = show_host_create();
    expect(host != nullptr, "create ShowHost");
    ShowHostCastPipelineTraceState initial_trace{};
    initial_trace.struct_size = sizeof(initial_trace);
    expect(show_host_get_cast_pipeline_trace_state(host, &initial_trace)
            && initial_trace.enabled == 0
            && initial_trace.latest_sequence == 0,
        "detailed Cast trace is disabled by default");
    expect(show_host_set_cast_pipeline_trace_enabled(host, true),
        "enable detailed Cast trace");
    expect(show_host_start_cast_stream(host, 0), "start Unified Cast stream");
    expect(show_host_get_active_video_decoder_owners(host) == 0,
        "idle Cast unexpectedly owns a video source");
    const auto port = show_host_get_cast_stream_port(host);
    expect(port != 0, "Cast stream bound port");
    ShowHostCastCapabilityReport capability{};
    capability.struct_size = sizeof(capability);
    expect(show_host_get_cast_capability_report(host, &capability)
            && capability.report_valid,
        "query startup Cast capability report");
    expect(capability.device_healthy.attempted
            && capability.even_420_geometry.attempted,
        "Cast capability gate did not run");
    expect(capability.backend_active,
        "successful Cast startup publishes its actual active backend");
    expect(capability.force_keyframe_control
                == SHOW_CAST_KEYFRAME_CONTROL_SUPPORTED
            || capability.force_keyframe_control
                == SHOW_CAST_KEYFRAME_CONTROL_UNSUPPORTED,
        "active encoder did not publish force-keyframe capability");
    expect(capability.backend != SHOW_CAST_BACKEND_NATIVE_NV12_VP_B,
        "runtime must not publish the not-yet-implemented backend B");
    if (capability.backend == SHOW_CAST_BACKEND_COMPUTE_NV12_A) {
        expect(capability.native_nv12_source.attempted
                && capability.native_nv12_source.supported
                && capability.nv12_plane_srvs.supported
                && capability.nv12_plane_uavs.supported
                && capability.nv12_video_processor.supported
                && capability.dxgi_h264_encoder.supported
                && capability.encoder_retains_samples_async,
            "active backend A must publish every required capability");
    } else {
        expect(capability.backend == SHOW_CAST_BACKEND_COMPATIBILITY_C,
            "startup must publish an implemented Cast backend");
    }

    TsClient client;
    expect(client.start(port), "connect Cast TS client");
    std::this_thread::sleep_for(1500ms);
    auto before_first_load = client.counts();
    expect(before_first_load.video > 0 && before_first_load.audio > 0,
        "empty-queue Cast emits continuous idle program");

    expect(show_host_load_with_options(host,
            argv[1], argv[2], argv[3], argv[4],
            SHOW_CLOCK_AUDIO_MASTER, SHOW_TRANSITION_HARD),
        "cold first-song load");
    show_host_play(host);
    std::this_thread::sleep_for(5s);
    expect(show_host_get_active_video_decoder_owners(host) == 1,
        "Cast playback did not reduce video decoding to one source owner");
    auto first_playing = client.counts();
    expect_progress(before_first_load, first_playing,
        "cold first-song playback");

    // Intentionally do not call prepare_next: this covers the unprepared song
    // transition that previously left receivers loading forever.
    // Host load must never wait for the Cast worker. On a slow progressive
    // source that handoff blocked the calling thread for seconds and froze the
    // Flutter UI while the Cast output kept playing.
    const auto timed_load = [&](const wchar_t* video,
                                const wchar_t* lyric,
                                const wchar_t* vocal,
                                const wchar_t* accompaniment,
                                int clock_mode,
                                int transition_mode,
                                const char* label) {
        const auto switch_started = std::chrono::steady_clock::now();
        expect(show_host_load_with_options(host,
                video, lyric, vocal, accompaniment,
                clock_mode, transition_mode),
            label);
        const auto switch_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - switch_started).count();
        std::cout << "CAST_SWITCH_LOAD_MS " << switch_ms << std::endl;
        expect(switch_ms < 2000,
            "next-song load blocked on the Cast worker");
    };
    // Rapid replacement forces an old frame, an intermediate publication and
    // the final publication to overlap. Each frame must retain the immutable
    // program/audio snapshot it acquired instead of reading host-owned values
    // that the following load is replacing.
    timed_load(argv[5], argv[6], argv[7], argv[8],
        SHOW_CLOCK_VIDEO_MASTER, SHOW_TRANSITION_SEAMLESS,
        "unprepared video-master next-song load");
    timed_load(argv[1], argv[2], argv[3], argv[4],
        SHOW_CLOCK_AUDIO_MASTER, SHOW_TRANSITION_HARD,
        "rapid first-song replacement load");
    timed_load(argv[5], argv[6], argv[7], argv[8],
        SHOW_CLOCK_VIDEO_MASTER, SHOW_TRANSITION_SEAMLESS,
        "rapid final next-song replacement load");
    show_host_play(host);
    std::this_thread::sleep_for(6s);
    auto second_playing = client.counts();
    expect_progress(first_playing, second_playing,
        "unprepared song switch");
    expect(show_host_get_active_video_decoder_owners(host) == 1,
        "video-master Cast reactivated a hidden MediaEngine decoder");

    show_host_pause(host);
    std::this_thread::sleep_for(2s);
    auto paused = client.counts();
    expect_progress(second_playing, paused,
        "paused broadcast keepalive");
    expect(show_host_get_state(host) == SHOW_STATE_PAUSED,
        "Cast pause did not publish paused state");
    expect(!show_host_is_buffering(host),
        "Cast pause remained reported as video buffering");
    const auto paused_position = show_host_get_position(host);

    // Resume the same program without a seek. This is deliberately separate
    // from the reconnect/replay check below: pause/resume must retain the
    // current decoder queue and must not get stuck behind a cold-start gate.
    show_host_play(host);
    const auto resume_deadline =
        std::chrono::steady_clock::now() + 5s;
    bool resumed{};
    while (std::chrono::steady_clock::now() < resume_deadline) {
        if (show_host_get_state(host) == SHOW_STATE_PLAYING
                && !show_host_is_buffering(host)
                && show_host_get_position(host) > paused_position + 0.05) {
            resumed = true;
            break;
        }
        std::this_thread::sleep_for(20ms);
    }
    expect(resumed,
        "Cast pause resume remained trapped in video buffering");
    const auto after_resume = client.counts();
    expect_progress(paused, after_resume,
        "resumed broadcast output");

    client.stop();
    std::this_thread::sleep_for(500ms);
    TsClient reconnected;
    expect(reconnected.start(port),
        "receiver reconnects without restarting Cast");
    show_host_seek(host, 0.0);
    show_host_play(host);
    std::this_thread::sleep_for(3s);
    auto replayed = reconnected.counts();
    expect(replayed.video > 0 && replayed.audio > 0,
        "reconnected receiver gets replay program");
    expect(client.malformed_packets() == 0,
        "first TS client remained packet-aligned");
    expect(client.pts_regressions() == 0,
        "first TS client saw monotonic audio/video PTS");
    expect(client.max_video_pts_gap() < 180000,
        "video transport has no gap above two seconds");
    expect(client.max_audio_pts_gap() < 180000,
        "audio transport has no gap above two seconds");
    const auto max_av_delta = client.max_nearest_av_pts_delta();
    std::cout << "Cast max nearest H264/AAC PTS delta: "
              << (static_cast<double>(max_av_delta) / 90.0)
              << " ms; max callback emission lead: "
              << (static_cast<double>(client.max_emission_pts_lead()) / 90.0)
              << " ms" << std::endl;
    expect(reconnected.malformed_packets() == 0,
        "reconnected TS client remained packet-aligned");
    expect(reconnected.pts_regressions() == 0,
        "reconnected TS client saw monotonic audio/video PTS");

    ShowHostCastPipelineStats pipeline{};
    pipeline.struct_size = sizeof(pipeline);
    expect(show_host_get_cast_pipeline_stats(host, &pipeline),
        "query event-driven Cast diagnostics");
    expect(pipeline.qpc_frequency != 0,
        "Cast diagnostics expose a QPC frequency");
    expect(pipeline.events[SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT].count > 0,
        "Cast diagnostics observed encoder input");
    expect(pipeline.events[SHOW_CAST_EVENT_ENCODER_OUTPUT].count > 0,
        "Cast diagnostics observed encoder output");
    expect(pipeline.events[SHOW_CAST_EVENT_TS_APPEND].count > 0,
        "Cast diagnostics observed TS append");
    expect(pipeline.events[SHOW_CAST_EVENT_HTTP_FIRST_BYTE].count > 0,
        "Cast diagnostics observed HTTP first byte");
    const auto event_ms = [&pipeline](std::uint32_t event,
            bool duration) {
        const auto ticks = duration
            ? pipeline.events[event].maximum_duration_qpc
            : pipeline.events[event].maximum_gap_qpc;
        return 1000.0 * static_cast<double>(ticks)
            / static_cast<double>(pipeline.qpc_frequency);
    };
    std::cout << "Cast pipeline diagnostics: decoded="
              << pipeline.decoded_frames
              << " repeated=" << pipeline.repeated_frames
              << " vp=" << pipeline.video_processor_passes
              << " compute=" << pipeline.compute_dispatches
              << " copies=" << pipeline.gpu_copies
              << " flushes=" << pipeline.explicit_flushes
              << " encoder_drops=" << pipeline.encoder_drops
              << " missed_slots=" << pipeline.missed_output_slots
              << " decoder_underflows=" << pipeline.decoder_underflows
              << " client_rebases=" << pipeline.client_rebases
              << std::endl;
    std::cout << "Cast pipeline maxima: normalize="
              << event_ms(SHOW_CAST_EVENT_NORMALIZE, true)
              << " ms overlay="
              << event_ms(SHOW_CAST_EVENT_OVERLAY_DRAW, true)
              << " ms compose="
              << event_ms(SHOW_CAST_EVENT_NV12_COMPOSE, true)
              << " ms encoder-in-gap="
              << event_ms(SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT, false)
              << " ms encoder-out-gap="
              << event_ms(SHOW_CAST_EVENT_ENCODER_OUTPUT, false)
              << " ms http-send-gap="
              << event_ms(SHOW_CAST_EVENT_HTTP_SEND, false)
              << " ms http-send-block="
              << event_ms(SHOW_CAST_EVENT_HTTP_SEND, true)
              << " ms" << std::endl;
    expect(max_av_delta <= 4500,
        "Cast H264/AAC PTS delta exceeds 50 ms");

    ShowHostCastPipelineTraceState trace_state{};
    trace_state.struct_size = sizeof(trace_state);
    expect(show_host_get_cast_pipeline_trace_state(host, &trace_state)
            && trace_state.enabled != 0
            && trace_state.latest_sequence > 0
            && trace_state.capacity > 0,
        "query detailed Cast trace state");
    std::vector<ShowHostCastPipelineTraceRecord> trace(trace_state.capacity);
    const auto trace_count = show_host_read_cast_pipeline_trace(
        host, trace.data(), static_cast<std::uint32_t>(trace.size()), 0);
    expect(trace_count > 0, "read detailed Cast trace");
    bool saw_encoder_input{};
    bool saw_encoder_output{};
    bool saw_ts_append{};
    bool saw_http_send{};
    bool saw_decoder_relation{};
    bool saw_normalize_relation{};
    bool saw_encoder_payload{};
    bool saw_ts_video{};
    bool saw_ts_audio{};
    bool saw_http_connection{};
    bool saw_gpu_copy{};
    bool saw_nv12_compose{};
    bool saw_fixed_flush_mode{};
    bool saw_keyframe_request{};
    bool saw_keyframe_request_applied{};
    bool saw_keyframe_request_unsupported{};
    bool saw_keyframe_request_failed{};
    std::vector<std::uint64_t> applied_keyframe_inputs;
    std::vector<std::uint64_t> keyframe_outputs;
    std::uint64_t maximum_keyframe_access_unit_bytes{};
    std::uint64_t maximum_delta_access_unit_bytes{};
    std::uint64_t maximum_video_ts_append_bytes{};
    std::uint64_t maximum_audio_ts_append_bytes{};
    std::uint64_t maximum_http_send_batch_bytes{};
    bool all_ts_appends_packet_aligned{true};
    bool all_http_sends_packet_aligned{true};
    std::uint64_t previous_sequence{};
    for (std::uint32_t index = 0; index < trace_count; ++index) {
        const auto& event = trace[index];
        expect(event.sequence > previous_sequence,
            "detailed Cast trace sequence is monotonic");
        expect(event.session_generation == pipeline.session_generation,
            "detailed Cast trace belongs to active session");
        previous_sequence = event.sequence;
        saw_encoder_input = saw_encoder_input
            || event.event == SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT;
        saw_encoder_output = saw_encoder_output
            || event.event == SHOW_CAST_EVENT_ENCODER_OUTPUT;
        saw_ts_append = saw_ts_append
            || event.event == SHOW_CAST_EVENT_TS_APPEND;
        saw_http_send = saw_http_send
            || event.event == SHOW_CAST_EVENT_HTTP_SEND;
        saw_decoder_relation = saw_decoder_relation
            || (event.event == SHOW_CAST_EVENT_DECODER_SAMPLE_READY
                && event.related_id != 0);
        saw_normalize_relation = saw_normalize_relation
            || (event.event == SHOW_CAST_EVENT_NORMALIZE
                && event.related_id != 0
                && event.value != 0);
        saw_encoder_payload = saw_encoder_payload
            || (event.event == SHOW_CAST_EVENT_ENCODER_OUTPUT
                && event.related_id != 0
                && event.value != 0);
        saw_ts_video = saw_ts_video
            || (event.event == SHOW_CAST_EVENT_TS_APPEND
                && event.related_id == 0
                && event.value > 0);
        saw_ts_audio = saw_ts_audio
            || (event.event == SHOW_CAST_EVENT_TS_APPEND
                && event.related_id == 1
                && event.value > 0);
        saw_http_connection = saw_http_connection
            || (event.event == SHOW_CAST_EVENT_HTTP_SEND
                && event.related_id != 0
                && event.value > 0);
        saw_gpu_copy = saw_gpu_copy
            || event.event == SHOW_CAST_EVENT_GPU_COPY;
        if (event.event == SHOW_CAST_EVENT_NV12_COMPOSE) {
            saw_nv12_compose = true;
            saw_fixed_flush_mode = saw_fixed_flush_mode
                || event.value == 1 || event.value == 2;
        }
        if (event.event == SHOW_CAST_EVENT_ENCODER_PROCESS_INPUT
                && (event.value
                    & SHOW_CAST_ENCODER_INPUT_KEYFRAME_REQUESTED) != 0) {
            saw_keyframe_request = true;
            if ((event.value
                    & SHOW_CAST_ENCODER_INPUT_KEYFRAME_APPLIED) != 0) {
                saw_keyframe_request_applied = true;
                applied_keyframe_inputs.push_back(event.correlation_id);
            }
            saw_keyframe_request_unsupported =
                saw_keyframe_request_unsupported
                || (event.value
                    & SHOW_CAST_ENCODER_INPUT_KEYFRAME_UNSUPPORTED) != 0;
            saw_keyframe_request_failed = saw_keyframe_request_failed
                || (event.value
                    & SHOW_CAST_ENCODER_INPUT_KEYFRAME_FAILED) != 0;
        }
        if (event.event == SHOW_CAST_EVENT_ENCODER_OUTPUT
                && event.value < 0) {
            keyframe_outputs.push_back(event.correlation_id);
            maximum_keyframe_access_unit_bytes = std::max(
                maximum_keyframe_access_unit_bytes,
                static_cast<std::uint64_t>(-event.value));
        } else if (event.event == SHOW_CAST_EVENT_ENCODER_OUTPUT
                && event.value > 0) {
            maximum_delta_access_unit_bytes = std::max(
                maximum_delta_access_unit_bytes,
                static_cast<std::uint64_t>(event.value));
        }
        if (event.event == SHOW_CAST_EVENT_TS_APPEND && event.value > 0) {
            const auto bytes = static_cast<std::uint64_t>(event.value);
            all_ts_appends_packet_aligned = all_ts_appends_packet_aligned
                && bytes % MpegTsMuxer::kPacketSize == 0;
            if (event.related_id == 0) {
                maximum_video_ts_append_bytes = std::max(
                    maximum_video_ts_append_bytes, bytes);
            } else if (event.related_id == 1) {
                maximum_audio_ts_append_bytes = std::max(
                    maximum_audio_ts_append_bytes, bytes);
            }
        }
        if (event.event == SHOW_CAST_EVENT_HTTP_SEND && event.value > 0) {
            const auto bytes = static_cast<std::uint64_t>(event.value);
            maximum_http_send_batch_bytes = std::max(
                maximum_http_send_batch_bytes, bytes);
            all_http_sends_packet_aligned = all_http_sends_packet_aligned
                && bytes % MpegTsMuxer::kPacketSize == 0;
        }
    }
    expect(saw_encoder_input && saw_encoder_output
            && saw_ts_append && saw_http_send,
        "detailed trace spans encoder through HTTP output");
    expect(saw_decoder_relation && saw_normalize_relation
            && saw_encoder_payload && saw_ts_video && saw_ts_audio
            && saw_http_connection,
        "detailed trace exposes cross-stage correlation fields");
    expect(saw_keyframe_request,
        "program handoff did not request a keyframe");
    expect(!saw_keyframe_request_failed,
        "encoder advertised force-keyframe support but rejected a request");
    expect(saw_keyframe_request_applied
            || saw_keyframe_request_unsupported,
        "keyframe request has no explicit driver result");
    expect(all_ts_appends_packet_aligned,
        "Cast TS append split an MPEG-TS packet");
    expect(all_http_sends_packet_aligned,
        "Cast HTTP send split an MPEG-TS packet");
    expect(maximum_http_send_batch_bytes <= 64U * 1024U,
        "Cast HTTP send batch exceeds the bounded live-stream budget");
    std::cout << "Cast transport burst maxima: keyframe-au="
              << maximum_keyframe_access_unit_bytes
              << " B delta-au=" << maximum_delta_access_unit_bytes
              << " B video-ts=" << maximum_video_ts_append_bytes
              << " B audio-ts=" << maximum_audio_ts_append_bytes
              << " B http-batch=" << maximum_http_send_batch_bytes
              << " B" << std::endl;
    if (saw_keyframe_request_applied) {
        std::cout << "Cast forced-keyframe inputs:";
        for (const auto frame : applied_keyframe_inputs) {
            std::cout << ' ' << frame;
        }
        std::cout << "; clean-point outputs:";
        for (const auto frame : keyframe_outputs) {
            std::cout << ' ' << frame;
        }
        std::cout << std::endl;
        for (const auto requested_frame : applied_keyframe_inputs) {
            const auto output = std::lower_bound(
                keyframe_outputs.begin(),
                keyframe_outputs.end(),
                requested_frame);
            expect(output != keyframe_outputs.end()
                    && *output - requested_frame <= 2,
                "applied keyframe request produced no nearby clean point");
        }
        expect(applied_keyframe_inputs.size() <= 8,
            "program handoff generated a force-keyframe request storm");
    }
    ShowHostCastCapabilityReport runtime_capability{};
    runtime_capability.struct_size = sizeof(runtime_capability);
    expect(show_host_get_cast_capability_report(host, &runtime_capability)
            && runtime_capability.force_keyframe_control
                != SHOW_CAST_KEYFRAME_CONTROL_FAILED,
        "runtime force-keyframe control entered a failed state");
    if (capability.backend == SHOW_CAST_BACKEND_COMPUTE_NV12_A) {
        expect(saw_gpu_copy,
            "backend A trace must expose its encoder-ready NV12 copy");
        expect(pipeline.compute_dispatches == 0 || saw_nv12_compose,
            "backend A compute work must expose an NV12 compose event");
        expect(pipeline.explicit_flushes == 0,
            "backend A must not inherit compatibility-path per-frame flushes");
    } else {
        expect(saw_nv12_compose && saw_fixed_flush_mode,
            "backend C trace must expose its fixed flush mode");
    }

    run_cast_soak(
        host, reconnected, runtime_capability, soak_duration);

    show_host_stop_cast_stream(host);
    expect(show_host_get_active_video_decoder_owners(host) == 1,
        "stop-Cast did not restore exactly one MediaEngine video owner");
    ShowHostCastCapabilityReport stopped_capability{};
    stopped_capability.struct_size = sizeof(stopped_capability);
    expect(show_host_get_cast_capability_report(host, &stopped_capability)
            && stopped_capability.report_valid
            && !stopped_capability.backend_active,
        "stopped Cast report keeps evidence but marks backend inactive");
    reconnected.stop();

    // Exercise the inverse lifecycle as well: take over a song that is already
    // running through MediaEngine. The owner count must move from one decoder
    // to one decoder, never through a persistent two-decoder Cast topology.
    const auto local_position = show_host_get_position(host);
    std::this_thread::sleep_for(250ms);
    expect(show_host_get_position(host) >= local_position,
        "restored local playback position regressed before hot takeover");
    auto native_takeover_source =
        show_host_create_stage_texture_source(host);
    expect(native_takeover_source != nullptr,
        "create Native Preview source before hot Cast takeover");
    show_host_stage_texture_source_set_active(
        native_takeover_source, true);
    std::this_thread::sleep_for(500ms);
    expect(show_host_prepare_next(host,
            argv[1], argv[2], argv[3], argv[4],
            SHOW_CLOCK_AUDIO_MASTER),
        "Native standby request before hot Cast takeover");
    // Let the Native path open and retain its bounded first frame. Cast must
    // asynchronously idle that private session before taking its decoder
    // lease rather than decoding both programs behind the broadcast.
    std::this_thread::sleep_for(750ms);
    expect(show_host_start_cast_stream(host, 0),
        "hot takeover of an already-loaded local program");
    show_host_destroy_stage_texture_source(native_takeover_source);
    expect(show_host_get_active_video_decoder_owners(host) == 1,
        "hot Cast takeover retained both video decoders");
    const auto hot_port = show_host_get_cast_stream_port(host);
    expect(hot_port != 0, "hot Cast takeover bound port");
    TsClient hot_client;
    expect(hot_client.start(hot_port), "connect after hot Cast takeover");
    std::this_thread::sleep_for(3s);
    const auto hot_counts = hot_client.counts();
    expect(hot_counts.video > 0 && hot_counts.audio > 0,
        "hot Cast takeover did not emit both streams");
    expect(show_host_get_position(host) >= local_position,
        "hot Cast takeover rewound the program clock");
    hot_client.stop();
    show_host_stop_cast_stream(host);
    expect(show_host_get_active_video_decoder_owners(host) == 1,
        "second stop-Cast did not restore one MediaEngine owner");

    show_host_destroy(host);
    std::cout << "Unified ShowHost Cast smoke passed: "
              << replayed.video << " video packets, "
              << replayed.audio << " audio packets, max A/V delta "
              << (static_cast<double>(max_av_delta) / 90.0)
              << " ms.\n";
    return 0;
}

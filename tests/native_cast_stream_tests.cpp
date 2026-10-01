#include "../apps/show_host/cast/native_cast_stream.h"
#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::uint16_t pid_at(const std::vector<std::uint8_t>& data, std::size_t packet) {
    const auto offset = packet * MpegTsMuxer::kPacketSize;
    return static_cast<std::uint16_t>(
        ((data[offset + 1] & 0x1FU) << 8U) | data[offset + 2]);
}

void expect_packets(const std::vector<std::uint8_t>& data) {
    expect(!data.empty(), "TS output should not be empty");
    expect(data.size() % MpegTsMuxer::kPacketSize == 0,
        "TS output must be 188-byte aligned");
    for (std::size_t offset = 0; offset < data.size();
            offset += MpegTsMuxer::kPacketSize) {
        expect(data[offset] == 0x47, "TS sync byte mismatch");
    }
}

std::vector<std::uint8_t> read_available(NativeCastStream& stream) {
    auto cursor = stream.cursor_from_head();
    std::vector<std::uint8_t> out(stream.buffered_bytes());
    const auto result = stream.read(
        cursor, out.data(), out.size(), std::chrono::milliseconds(1));
    out.resize(result.bytes);
    return out;
}

void keyframes_prepend_program_tables() {
    NativeCastStream stream;
    const std::vector<std::uint8_t> keyframe{
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21};
    stream.append_h264_access_unit(
        keyframe.data(), keyframe.size(), 90000, true);

    const auto out = read_available(stream);
    expect_packets(out);
    expect(pid_at(out, 0) == MpegTsMuxer::kPatPid,
        "keyframe should prepend PAT");
    expect(pid_at(out, 1) == MpegTsMuxer::kPmtPid,
        "keyframe should prepend PMT");
    expect(pid_at(out, 2) == MpegTsMuxer::kVideoPid,
        "keyframe should write video PID after tables");
}

void audio_and_video_share_one_stream() {
    NativeCastStream stream;
    const std::vector<std::uint8_t> video{
        0x00, 0x00, 0x01, 0x41, 0x9A, 0x10};
    const std::vector<std::uint8_t> audio{
        0xFF, 0xF1, 0x50, 0x80, 0x02, 0x9F, 0xFC, 0x11, 0x22};

    stream.append_h264_access_unit(video.data(), video.size(), 180000, false);
    stream.append_aac_adts_frame(audio.data(), audio.size(), 181920);

    const auto out = read_available(stream);
    expect_packets(out);
    expect(pid_at(out, 0) == MpegTsMuxer::kVideoPid,
        "first packet should be video");
    expect(pid_at(out, 1) == MpegTsMuxer::kPatPid,
        "first audio should refresh PAT");
    expect(pid_at(out, 2) == MpegTsMuxer::kPmtPid,
        "first audio should refresh PMT");
    expect(pid_at(out, 3) == MpegTsMuxer::kAudioPid,
        "audio should follow refreshed tables");
}

void late_clients_start_on_decodable_keyframe() {
    NativeCastStream stream;
    const std::vector<std::uint8_t> first_keyframe{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x40, 0x2A,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
    };
    const std::vector<std::uint8_t> delta{
        0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x10,
    };
    const std::vector<std::uint8_t> later_keyframe{
        0x00, 0x00, 0x00, 0x01, 0x65, 0x99, 0x55, 0x11,
    };
    stream.append_h264_access_unit(
        first_keyframe.data(), first_keyframe.size(), 0, true);
    stream.append_h264_access_unit(
        delta.data(), delta.size(), 45000, false);
    stream.append_h264_access_unit(
        later_keyframe.data(), later_keyframe.size(), 90000, true);

    auto cursor = stream.cursor_from_latest_keyframe();
    std::vector<std::uint8_t> out(stream.buffered_bytes());
    const auto result = stream.read(
        cursor, out.data(), out.size(), std::chrono::milliseconds(1));
    out.resize(result.bytes);
    expect_packets(out);
    expect(pid_at(out, 0) == MpegTsMuxer::kPatPid,
        "late client should begin at keyframe PAT");
    expect(pid_at(out, 1) == MpegTsMuxer::kPmtPid,
        "late client should begin at keyframe PMT");
    const std::vector<std::uint8_t> sps{0x00, 0x00, 0x00, 0x01, 0x67};
    const std::vector<std::uint8_t> pps{0x00, 0x00, 0x00, 0x01, 0x68};
    expect(std::search(out.begin(), out.end(), sps.begin(), sps.end())
            != out.end(),
        "late keyframe should repeat cached SPS");
    expect(std::search(out.begin(), out.end(), pps.begin(), pps.end())
            != out.end(),
        "late keyframe should repeat cached PPS");
}

void reset_notifies_existing_readers() {
    NativeCastStream stream;
    auto cursor = stream.cursor_from_tail();
    stream.reset();

    std::uint8_t out[188]{};
    const auto result = stream.read(
        cursor, out, sizeof(out), std::chrono::milliseconds(1));
    expect(result.reset, "reader should observe stream reset");
    expect(result.bytes == 0, "reset without frames should not emit bytes");
}

void lagging_clients_rebase_to_live_keyframe() {
    NativeCastStream stream;
    const std::vector<std::uint8_t> first_keyframe{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x4D, 0x40, 0x2A,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88,
    };
    const std::vector<std::uint8_t> delta(1024, 0x41);
    const std::vector<std::uint8_t> next_keyframe{
        0x00, 0x00, 0x00, 0x01, 0x65, 0x99,
    };
    stream.append_h264_access_unit(
        first_keyframe.data(), first_keyframe.size(), 0, true);
    auto cursor = stream.cursor_from_latest_keyframe();
    stream.append_h264_access_unit(
        delta.data(), delta.size(), 45000, false);
    stream.append_h264_access_unit(
        next_keyframe.data(), next_keyframe.size(), 90000, true);

    expect(stream.rebase_to_latest_keyframe_if_lagging(cursor, 1),
        "lagging client should jump to latest keyframe");
    const auto latest = stream.cursor_from_latest_keyframe();
    expect(cursor.generation == latest.generation
            && cursor.sequence == latest.sequence,
        "lagging client did not reach live keyframe");
}

void append_events_are_observable_without_polling() {
    CastPipelineDiagnostics diagnostics;
    NativeCastStream stream;
    stream.set_diagnostics(&diagnostics);
    const std::vector<std::uint8_t> video{
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21};
    const std::vector<std::uint8_t> audio{
        0xFF, 0xF1, 0x50, 0x80, 0x02, 0x9F, 0xFC, 0x11, 0x22};
    stream.append_h264_access_unit(
        video.data(), video.size(), 90000, true);
    stream.append_aac_adts_frame(
        audio.data(), audio.size(), 91920);
    const auto snapshot = diagnostics.snapshot();
    expect(snapshot.event(CastPipelineEvent::ts_append).count == 2,
        "TS append diagnostics should follow muxer events");
    expect(snapshot.event(CastPipelineEvent::ts_append)
            .last_correlation_id == 91920,
        "TS append diagnostics should retain the latest PTS");
}

}  // namespace

int main() {
    keyframes_prepend_program_tables();
    audio_and_video_share_one_stream();
    late_clients_start_on_decodable_keyframe();
    reset_notifies_existing_readers();
    lagging_clients_rebase_to_live_keyframe();
    append_events_are_observable_without_polling();
    std::cout << "NativeCastStream test passed.\n";
    return 0;
}

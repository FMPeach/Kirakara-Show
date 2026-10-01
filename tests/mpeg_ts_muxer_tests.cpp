#include "../apps/show_host/cast/mpeg_ts_muxer.h"

#include <algorithm>
#include <array>
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

bool payload_unit_start(
        const std::vector<std::uint8_t>& data, std::size_t packet) {
    return (data[packet * MpegTsMuxer::kPacketSize + 1] & 0x40U) != 0;
}

bool has_payload(
        const std::vector<std::uint8_t>& data, std::size_t packet) {
    const auto control = static_cast<std::uint8_t>(
        (data[packet * MpegTsMuxer::kPacketSize + 3] >> 4U) & 0x03U);
    return control == 0x01U || control == 0x03U;
}

bool discontinuity_indicator(
        const std::vector<std::uint8_t>& data, std::size_t packet) {
    const auto base = packet * MpegTsMuxer::kPacketSize;
    const auto control = static_cast<std::uint8_t>(
        (data[base + 3] >> 4U) & 0x03U);
    return (control == 0x02U || control == 0x03U)
        && data[base + 4] > 0U
        && (data[base + 5] & 0x80U) != 0;
}

std::uint8_t continuity(
        const std::vector<std::uint8_t>& data, std::size_t packet) {
    return data[packet * MpegTsMuxer::kPacketSize + 3] & 0x0FU;
}

std::size_t payload_offset(
        const std::vector<std::uint8_t>& data, std::size_t packet) {
    const auto base = packet * MpegTsMuxer::kPacketSize;
    const auto afc = (data[base + 3] >> 4U) & 0x03U;
    if (afc == 0x01U) return base + 4;
    if (afc == 0x03U) return base + 5 + data[base + 4];
    return base + MpegTsMuxer::kPacketSize;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U)
                ? (crc << 1U) ^ 0x04C11DB7U
                : (crc << 1U);
        }
    }
    return crc;
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

std::vector<std::uint8_t> section_at(
        const std::vector<std::uint8_t>& data, std::size_t packet) {
    const auto base = packet * MpegTsMuxer::kPacketSize;
    const auto pointer = data[base + 4];
    const auto start = base + 5 + pointer;
    const auto length = static_cast<std::uint16_t>(
        ((data[start + 1] & 0x0FU) << 8U) | data[start + 2]);
    return {data.begin() + static_cast<std::ptrdiff_t>(start),
        data.begin() + static_cast<std::ptrdiff_t>(start + 3 + length)};
}

std::int64_t decode_pts(const std::uint8_t* data) {
    return (static_cast<std::int64_t>((data[0] >> 1U) & 0x07U) << 30U)
        | (static_cast<std::int64_t>(data[1]) << 22U)
        | (static_cast<std::int64_t>((data[2] >> 1U) & 0x7FU) << 15U)
        | (static_cast<std::int64_t>(data[3]) << 7U)
        | static_cast<std::int64_t>((data[4] >> 1U) & 0x7FU);
}

std::vector<std::uint8_t> collect_pid_payload(
        const std::vector<std::uint8_t>& data,
        std::uint16_t pid) {
    std::vector<std::uint8_t> payload;
    for (std::size_t packet = 0;
            packet < data.size() / MpegTsMuxer::kPacketSize; ++packet) {
        if (pid_at(data, packet) != pid) continue;
        const auto start = payload_offset(data, packet);
        const auto end = (packet + 1U) * MpegTsMuxer::kPacketSize;
        if (start < end) {
            payload.insert(payload.end(),
                data.begin() + static_cast<std::ptrdiff_t>(start),
                data.begin() + static_cast<std::ptrdiff_t>(end));
        }
    }
    return payload;
}

void test_tables() {
    MpegTsMuxer muxer;
    std::vector<std::uint8_t> out;
    muxer.append_tables(out);

    expect_packets(out);
    expect(out.size() == MpegTsMuxer::kPacketSize * 2,
        "PAT/PMT should fit in two packets");
    expect(pid_at(out, 0) == MpegTsMuxer::kPatPid, "PAT PID mismatch");
    expect(pid_at(out, 1) == MpegTsMuxer::kPmtPid, "PMT PID mismatch");
    expect(payload_unit_start(out, 0), "PAT must set PUSI");
    expect(payload_unit_start(out, 1), "PMT must set PUSI");
    expect(continuity(out, 0) == 0, "PAT counter should start at zero");
    expect(continuity(out, 1) == 0, "PMT counter should start at zero");

    const auto pat = section_at(out, 0);
    const auto pmt = section_at(out, 1);
    expect(pat[0] == 0x00, "PAT table id mismatch");
    expect(pmt[0] == 0x02, "PMT table id mismatch");
    expect(crc32(pat.data(), pat.size()) == 0, "PAT CRC mismatch");
    expect(crc32(pmt.data(), pmt.size()) == 0, "PMT CRC mismatch");
    const auto pmt_pid = static_cast<std::uint16_t>(
        ((pat[10] & 0x1FU) << 8U) | pat[11]);
    expect(pmt_pid == MpegTsMuxer::kPmtPid, "PAT PMT PID mismatch");
    const auto pcr_pid = static_cast<std::uint16_t>(
        ((pmt[8] & 0x1FU) << 8U) | pmt[9]);
    expect(pcr_pid == MpegTsMuxer::kVideoPid, "PMT PCR PID mismatch");
    expect(pmt[12] == 0x1B, "PMT video stream type should be H.264");
    expect(std::find(pmt.begin(), pmt.end(), 0x0F) == pmt.end(),
        "video-only PMT must not advertise absent AAC");
}

void test_video_pes() {
    MpegTsMuxer muxer;
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> au(300);
    for (std::size_t i = 0; i < au.size(); ++i) {
        au[i] = static_cast<std::uint8_t>(i & 0xFFU);
    }
    constexpr std::int64_t pts = 90000 * 12 + 345;
    muxer.append_video_access_unit(au.data(), au.size(), pts, true, out);

    expect_packets(out);
    expect(pid_at(out, 0) == MpegTsMuxer::kPatPid,
        "keyframe should prepend PAT");
    expect(pid_at(out, 1) == MpegTsMuxer::kPmtPid,
        "keyframe should prepend PMT");
    expect(pid_at(out, 2) == MpegTsMuxer::kVideoPid,
        "video PID mismatch");
    expect(payload_unit_start(out, 2), "first video packet must set PUSI");
    const auto base = 2 * MpegTsMuxer::kPacketSize;
    expect(((out[base + 3] >> 4U) & 0x03U) == 0x03U,
        "video first packet should have adaptation field");
    expect(out[base + 4] >= 7, "video first packet should fit PCR");
    expect((out[base + 5] & 0x10U) != 0, "video first packet missing PCR");

    const auto payload = collect_pid_payload(out, MpegTsMuxer::kVideoPid);
    expect(payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01,
        "video PES prefix mismatch");
    expect(payload[3] == 0xE0, "video stream id mismatch");
    expect(payload[4] == 0x00 && payload[5] == 0x00,
        "video PES length should be unbounded");
    expect(decode_pts(payload.data() + 9) == pts, "video PTS mismatch");
    expect(std::search(payload.begin(), payload.end(), au.begin(), au.end())
            != payload.end(), "video payload should contain access unit");
}

void test_audio_pes() {
    MpegTsMuxer muxer;
    std::vector<std::uint8_t> out;
    std::vector<std::uint8_t> frame(32, 0xAA);
    constexpr std::int64_t pts = 45000;
    muxer.append_audio_frame(frame.data(), frame.size(), pts, out);

    expect_packets(out);
    expect(pid_at(out, 0) == MpegTsMuxer::kPatPid,
        "first audio frame should refresh PAT");
    expect(pid_at(out, 1) == MpegTsMuxer::kPmtPid,
        "first audio frame should advertise AAC in PMT");
    const auto pmt = section_at(out, 1);
    expect(std::find(pmt.begin(), pmt.end(), 0x0F) != pmt.end(),
        "audio-enabled PMT should advertise AAC ADTS");
    expect(pid_at(out, 2) == MpegTsMuxer::kAudioPid, "audio PID mismatch");
    expect(payload_unit_start(out, 2), "audio packet must set PUSI");
    const auto payload = collect_pid_payload(out, MpegTsMuxer::kAudioPid);
    expect(payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01,
        "audio PES prefix mismatch");
    expect(payload[3] == 0xC0, "audio stream id mismatch");
    const auto pes_length = static_cast<std::uint16_t>(
        (payload[4] << 8U) | payload[5]);
    expect(pes_length == frame.size() + 8U, "audio PES length mismatch");
    expect(decode_pts(payload.data() + 9) == pts, "audio PTS mismatch");
}

void test_pes_payload_boundaries() {
    constexpr std::array<std::size_t, 8> sizes{
        1U, 161U, 162U, 163U, 345U, 346U, 347U, 8192U};
    for (const auto size : sizes) {
        MpegTsMuxer muxer;
        std::vector<std::uint8_t> access_unit(size);
        for (std::size_t index = 0; index < size; ++index) {
            access_unit[index] = static_cast<std::uint8_t>(
                (index * 37U + size) & 0xFFU);
        }
        std::vector<std::uint8_t> out;
        muxer.append_video_access_unit(
            access_unit.data(), access_unit.size(), 123456, false, out);
        expect_packets(out);
        const auto payload = collect_pid_payload(
            out, MpegTsMuxer::kVideoPid);
        expect(payload.size() == 14U + access_unit.size(),
            "packet boundary changed reconstructed PES size");
        expect(std::equal(access_unit.begin(), access_unit.end(),
                payload.begin() + 14),
            "packet boundary changed access-unit bytes");
        expect(decode_pts(payload.data() + 9) == 123456,
            "packet boundary changed PTS");
    }
}

void test_client_continuity_survives_live_rebase() {
    MpegTsMuxer muxer;
    MpegTsContinuityRewriter continuity_rewriter;
    const std::vector<std::uint8_t> keyframe{
        0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21};
    const std::vector<std::uint8_t> delta{
        0x00, 0x00, 0x01, 0x41, 0x9A, 0x10};
    const std::vector<std::uint8_t> audio{
        0xFF, 0xF1, 0x50, 0x80, 0x02, 0x9F, 0xFC, 0x11, 0x22};

    std::vector<std::uint8_t> first;
    muxer.append_video_access_unit(
        keyframe.data(), keyframe.size(), 0, true, first);
    muxer.append_audio_frame(audio.data(), audio.size(), 1920, first);
    expect(continuity_rewriter.rewrite_packets(first.data(), first.size()),
        "initial client TS rewrite failed");

    std::array<std::uint8_t, 8192> last_continuity{};
    std::array<bool, 8192> seen{};
    for (std::size_t packet = 0;
            packet < first.size() / MpegTsMuxer::kPacketSize; ++packet) {
        if (!has_payload(first, packet)) continue;
        const auto pid = pid_at(first, packet);
        seen[pid] = true;
        last_continuity[pid] = continuity(first, packet);
    }

    std::vector<std::uint8_t> skipped;
    for (int i = 0; i < 20; ++i) {
        muxer.append_video_access_unit(
            delta.data(), delta.size(), 3000 + i * 1500,
            false, skipped);
        muxer.append_audio_frame(
            audio.data(), audio.size(), 3000 + i * 1920, skipped);
    }

    std::vector<std::uint8_t> resumed;
    muxer.append_video_access_unit(
        keyframe.data(), keyframe.size(), 90000, true, resumed);
    muxer.append_audio_frame(audio.data(), audio.size(), 91920, resumed);

    std::vector<std::uint8_t> marker;
    continuity_rewriter.append_discontinuity_packets(marker);
    expect_packets(marker);
    expect(continuity_rewriter.rewrite_packets(
            resumed.data(), resumed.size()),
        "rebased client TS rewrite failed");

    std::array<bool, 8192> marked{};
    for (std::size_t packet = 0;
            packet < marker.size() / MpegTsMuxer::kPacketSize; ++packet) {
        const auto pid = pid_at(marker, packet);
        marked[pid] = true;
        expect(discontinuity_indicator(marker, packet),
            "rebase marker should set discontinuity indicator");
        expect(continuity(marker, packet) == last_continuity[pid],
            "adaptation-only marker should retain previous counter");
    }

    std::array<bool, 8192> checked{};
    for (std::size_t packet = 0;
            packet < resumed.size() / MpegTsMuxer::kPacketSize; ++packet) {
        const auto pid = pid_at(resumed, packet);
        if (!has_payload(resumed, packet) || checked[pid]) continue;
        checked[pid] = true;
        expect(seen[pid] && marked[pid],
            "rebased PID should receive a discontinuity marker");
        expect(continuity(resumed, packet)
                == static_cast<std::uint8_t>(
                    (last_continuity[pid] + 1U) & 0x0FU),
            "rebased PID continuity should remain sequential");
    }

    expect(checked[MpegTsMuxer::kPatPid], "PAT was not checked");
    expect(checked[MpegTsMuxer::kPmtPid], "PMT was not checked");
    expect(checked[MpegTsMuxer::kVideoPid], "video PID was not checked");
    expect(checked[MpegTsMuxer::kAudioPid], "audio PID was not checked");
}

}  // namespace

int main() {
    test_tables();
    test_video_pes();
    test_audio_pes();
    test_pes_payload_boundaries();
    test_client_continuity_survives_live_rebase();
    std::cout << "MpegTsMuxer test passed.\n";
    return 0;
}

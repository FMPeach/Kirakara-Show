#include "mpeg_ts_muxer.h"

#include <algorithm>
#include <array>
#include <limits>

namespace {

constexpr std::uint8_t kStreamTypeH264 = 0x1B;
constexpr std::uint8_t kStreamTypeAacAdts = 0x0F;
constexpr std::uint8_t kVideoStreamId = 0xE0;
constexpr std::uint8_t kAudioStreamId = 0xC0;
constexpr std::uint16_t kProgramNumber = 1;
constexpr std::uint16_t kTransportStreamId = 1;

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

std::uint32_t mpeg_crc32(
        const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]) << 24U;
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80000000U) {
                crc = (crc << 1U) ^ 0x04C11DB7U;
            } else {
                crc <<= 1U;
            }
        }
    }
    return crc;
}

void append_crc(std::vector<std::uint8_t>& section) {
    append_u32(section, mpeg_crc32(section.data(), section.size()));
}

void write_pts(std::uint8_t* out, std::int64_t pts90k) {
    auto pts = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, pts90k)) & 0x1FFFFFFFFULL;
    out[0] = static_cast<std::uint8_t>(
        0x20U | (((pts >> 30U) & 0x07U) << 1U) | 0x01U);
    out[1] = static_cast<std::uint8_t>((pts >> 22U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>(
        (((pts >> 15U) & 0x7FU) << 1U) | 0x01U);
    out[3] = static_cast<std::uint8_t>((pts >> 7U) & 0xFFU);
    out[4] = static_cast<std::uint8_t>(
        ((pts & 0x7FU) << 1U) | 0x01U);
}

void write_pcr(std::uint8_t* dst, std::int64_t pts90k) {
    auto base = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, pts90k)) & 0x1FFFFFFFFULL;
    dst[0] = static_cast<std::uint8_t>((base >> 25U) & 0xFFU);
    dst[1] = static_cast<std::uint8_t>((base >> 17U) & 0xFFU);
    dst[2] = static_cast<std::uint8_t>((base >> 9U) & 0xFFU);
    dst[3] = static_cast<std::uint8_t>((base >> 1U) & 0xFFU);
    dst[4] = static_cast<std::uint8_t>(
        ((base & 0x01U) << 7U) | 0x7EU);
    dst[5] = 0x00;
}

std::vector<std::uint8_t> make_pat_section() {
    std::vector<std::uint8_t> section;
    section.reserve(16);
    section.push_back(0x00);  // table_id
    const std::uint16_t section_length = 13;
    section.push_back(static_cast<std::uint8_t>(
        0xB0U | ((section_length >> 8U) & 0x0FU)));
    section.push_back(static_cast<std::uint8_t>(section_length & 0xFFU));
    append_u16(section, kTransportStreamId);
    section.push_back(0xC1);  // reserved + version 0 + current_next
    section.push_back(0x00);  // section_number
    section.push_back(0x00);  // last_section_number
    append_u16(section, kProgramNumber);
    section.push_back(static_cast<std::uint8_t>(
        0xE0U | ((MpegTsMuxer::kPmtPid >> 8U) & 0x1FU)));
    section.push_back(static_cast<std::uint8_t>(
        MpegTsMuxer::kPmtPid & 0xFFU));
    append_crc(section);
    return section;
}

std::vector<std::uint8_t> make_pmt_section(
        bool include_audio, std::uint8_t version) {
    std::vector<std::uint8_t> section;
    section.reserve(32);
    section.push_back(0x02);  // table_id
    const std::uint16_t section_length = include_audio ? 23 : 18;
    section.push_back(static_cast<std::uint8_t>(
        0xB0U | ((section_length >> 8U) & 0x0FU)));
    section.push_back(static_cast<std::uint8_t>(section_length & 0xFFU));
    append_u16(section, kProgramNumber);
    section.push_back(static_cast<std::uint8_t>(
        0xC1U | ((version & 0x1FU) << 1U)));
    section.push_back(0x00);
    section.push_back(0x00);
    section.push_back(static_cast<std::uint8_t>(
        0xE0U | ((MpegTsMuxer::kVideoPid >> 8U) & 0x1FU)));
    section.push_back(static_cast<std::uint8_t>(
        MpegTsMuxer::kVideoPid & 0xFFU));
    section.push_back(0xF0);
    section.push_back(0x00);  // program_info_length

    section.push_back(kStreamTypeH264);
    section.push_back(static_cast<std::uint8_t>(
        0xE0U | ((MpegTsMuxer::kVideoPid >> 8U) & 0x1FU)));
    section.push_back(static_cast<std::uint8_t>(
        MpegTsMuxer::kVideoPid & 0xFFU));
    section.push_back(0xF0);
    section.push_back(0x00);

    if (include_audio) {
        section.push_back(kStreamTypeAacAdts);
        section.push_back(static_cast<std::uint8_t>(
            0xE0U | ((MpegTsMuxer::kAudioPid >> 8U) & 0x1FU)));
        section.push_back(static_cast<std::uint8_t>(
            MpegTsMuxer::kAudioPid & 0xFFU));
        section.push_back(0xF0);
        section.push_back(0x00);
    }

    append_crc(section);
    return section;
}

}  // namespace

void MpegTsMuxer::reset() {
    continuity_.fill(0);
    audio_enabled_ = false;
    pmt_version_ = 0;
}

void MpegTsMuxer::append_tables(std::vector<std::uint8_t>& out) {
    append_section_packet(kPatPid, make_pat_section(), out);
    append_section_packet(
        kPmtPid, make_pmt_section(audio_enabled_, pmt_version_), out);
}

void MpegTsMuxer::append_video_access_unit(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool keyframe,
        std::vector<std::uint8_t>& out) {
    if (!data || size == 0) return;
    if (keyframe) append_tables(out);
    append_pes_packets(
        kVideoPid, kVideoStreamId, data, size, pts90k, true, out);
}

void MpegTsMuxer::append_audio_frame(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        std::vector<std::uint8_t>& out) {
    if (!data || size == 0) return;
    if (!audio_enabled_) {
        audio_enabled_ = true;
        pmt_version_ = static_cast<std::uint8_t>(
            (pmt_version_ + 1U) & 0x1FU);
        append_tables(out);
    }
    append_pes_packets(
        kAudioPid, kAudioStreamId, data, size, pts90k, false, out);
}

void MpegTsMuxer::append_section_packet(
        std::uint16_t pid,
        const std::vector<std::uint8_t>& section,
        std::vector<std::uint8_t>& out) {
    if (section.size() + 5U > kPacketSize) return;
    const auto start = out.size();
    out.resize(start + kPacketSize, 0xFF);
    auto* packet = out.data() + start;
    packet[0] = 0x47;
    packet[1] = static_cast<std::uint8_t>(
        0x40U | ((pid >> 8U) & 0x1FU));
    packet[2] = static_cast<std::uint8_t>(pid & 0xFFU);
    packet[3] = static_cast<std::uint8_t>(
        0x10U | (next_counter(pid) & 0x0FU));
    packet[4] = 0x00;  // pointer_field
    std::copy(section.begin(), section.end(), packet + 5);
}

void MpegTsMuxer::append_pes_packets(
        std::uint16_t pid,
        std::uint8_t stream_id,
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool include_pcr,
        std::vector<std::uint8_t>& out) {
    constexpr std::size_t kPesHeaderSize = 14U;
    if (size > std::numeric_limits<std::size_t>::max() - kPesHeaderSize) {
        return;
    }
    std::array<std::uint8_t, kPesHeaderSize> pes_header{};
    pes_header[0] = 0x00;
    pes_header[1] = 0x00;
    pes_header[2] = 0x01;
    pes_header[3] = stream_id;

    const auto pes_payload_length = size + 8U;
    const bool use_unbounded_length =
        stream_id == kVideoStreamId
        || pes_payload_length > std::numeric_limits<std::uint16_t>::max();
    const auto pes_length = use_unbounded_length
        ? 0U : static_cast<std::uint16_t>(pes_payload_length);
    pes_header[4] = static_cast<std::uint8_t>(
        (pes_length >> 8U) & 0xFFU);
    pes_header[5] = static_cast<std::uint8_t>(pes_length & 0xFFU);
    pes_header[6] = 0x80;  // marker bits '10'
    pes_header[7] = 0x80;  // PTS only
    pes_header[8] = 0x05;  // PTS field length
    write_pts(pes_header.data() + 9U, pts90k);

    const auto pes_size = kPesHeaderSize + size;
    const auto first_payload_capacity = include_pcr ? 176U : 184U;
    const auto packet_count = pes_size <= first_payload_capacity
        ? 1U
        : 1U + (pes_size - first_payload_capacity + 183U) / 184U;
    if (packet_count <= (std::numeric_limits<std::size_t>::max()
            - out.size()) / kPacketSize) {
        out.reserve(out.size() + packet_count * kPacketSize);
    }
    std::size_t offset = 0;
    bool first_packet = true;
    while (offset < pes_size) {
        const auto start = out.size();
        out.resize(start + kPacketSize, 0xFF);
        auto* packet = out.data() + start;
        const bool with_pcr = include_pcr && first_packet;
        const auto max_payload = with_pcr ? 176U : 184U;
        auto payload_size = std::min<std::size_t>(
            max_payload, pes_size - offset);
        const bool needs_adaptation =
            with_pcr || payload_size < 184U;

        packet[0] = 0x47;
        packet[1] = static_cast<std::uint8_t>(
            (first_packet ? 0x40U : 0x00U) | ((pid >> 8U) & 0x1FU));
        packet[2] = static_cast<std::uint8_t>(pid & 0xFFU);
        packet[3] = static_cast<std::uint8_t>(
            (needs_adaptation ? 0x30U : 0x10U)
            | (next_counter(pid) & 0x0FU));

        std::size_t payload_offset = 4;
        if (needs_adaptation) {
            const auto adaptation_length =
                static_cast<std::uint8_t>(183U - payload_size);
            packet[4] = adaptation_length;
            payload_offset = 5U + adaptation_length;
            if (adaptation_length > 0U) {
                packet[5] = with_pcr ? 0x10 : 0x00;
            }
            if (with_pcr && adaptation_length >= 7U) {
                write_pcr(packet + 6, pts90k);
            }
        }

        auto remaining = payload_size;
        auto destination_offset = payload_offset;
        if (offset < kPesHeaderSize) {
            const auto header_bytes = std::min(
                remaining, kPesHeaderSize - offset);
            std::copy_n(pes_header.data() + offset,
                header_bytes, packet + destination_offset);
            remaining -= header_bytes;
            destination_offset += header_bytes;
        }
        if (remaining > 0) {
            const auto source_offset = offset + payload_size - remaining
                - kPesHeaderSize;
            std::copy_n(data + source_offset,
                remaining, packet + destination_offset);
        }
        offset += payload_size;
        first_packet = false;
    }
}

std::uint8_t MpegTsMuxer::next_counter(std::uint16_t pid) {
    if (pid >= continuity_.size()) return 0;
    const auto value = continuity_[pid] & 0x0FU;
    continuity_[pid] = static_cast<std::uint8_t>((value + 1U) & 0x0FU);
    return value;
}

void MpegTsContinuityRewriter::reset() {
    next_continuity_.fill(0);
    seen_.fill(false);
}

bool MpegTsContinuityRewriter::rewrite_packets(
        std::uint8_t* data, std::size_t size) {
    if (!data || size == 0 || size % MpegTsMuxer::kPacketSize != 0) {
        return false;
    }
    for (std::size_t offset = 0; offset < size;
            offset += MpegTsMuxer::kPacketSize) {
        if (data[offset] != 0x47) return false;
    }

    for (std::size_t offset = 0; offset < size;
            offset += MpegTsMuxer::kPacketSize) {
        auto* packet = data + offset;
        const auto pid = static_cast<std::uint16_t>(
            ((packet[1] & 0x1FU) << 8U) | packet[2]);
        if (pid >= next_continuity_.size()) continue;

        const auto adaptation_control =
            static_cast<std::uint8_t>((packet[3] >> 4U) & 0x03U);
        const bool has_payload = adaptation_control == 0x01U
            || adaptation_control == 0x03U;
        const auto continuity = has_payload
            ? next_continuity_[pid]
            : seen_[pid]
                ? static_cast<std::uint8_t>(
                    (next_continuity_[pid] + 15U) & 0x0FU)
                : 0U;
        packet[3] = static_cast<std::uint8_t>(
            (packet[3] & 0xF0U) | continuity);
        if (has_payload) {
            seen_[pid] = true;
            next_continuity_[pid] = static_cast<std::uint8_t>(
                (continuity + 1U) & 0x0FU);
        }
    }
    return true;
}

void MpegTsContinuityRewriter::append_discontinuity_packets(
        std::vector<std::uint8_t>& out) const {
    for (std::size_t pid = 0; pid < seen_.size(); ++pid) {
        if (!seen_[pid]) continue;

        const auto start = out.size();
        out.resize(start + MpegTsMuxer::kPacketSize, 0xFF);
        auto* packet = out.data() + start;
        packet[0] = 0x47;
        packet[1] = static_cast<std::uint8_t>((pid >> 8U) & 0x1FU);
        packet[2] = static_cast<std::uint8_t>(pid & 0xFFU);
        const auto previous_continuity = static_cast<std::uint8_t>(
            (next_continuity_[pid] + 15U) & 0x0FU);
        packet[3] = static_cast<std::uint8_t>(
            0x20U | previous_continuity);
        packet[4] = 183U;
        packet[5] = 0x80U;
    }
}

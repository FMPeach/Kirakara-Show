#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

class MpegTsMuxer {
public:
    static constexpr std::size_t kPacketSize = 188;
    static constexpr std::uint16_t kPatPid = 0x0000;
    static constexpr std::uint16_t kPmtPid = 0x0100;
    static constexpr std::uint16_t kVideoPid = 0x0101;
    static constexpr std::uint16_t kAudioPid = 0x0102;

    void reset();

    void append_tables(std::vector<std::uint8_t>& out);

    void append_video_access_unit(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool keyframe,
        std::vector<std::uint8_t>& out);

    void append_audio_frame(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        std::vector<std::uint8_t>& out);

private:
    void append_section_packet(
        std::uint16_t pid,
        const std::vector<std::uint8_t>& section,
        std::vector<std::uint8_t>& out);

    void append_pes_packets(
        std::uint16_t pid,
        std::uint8_t stream_id,
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool include_pcr,
        std::vector<std::uint8_t>& out);

    [[nodiscard]] std::uint8_t next_counter(std::uint16_t pid);

    std::array<std::uint8_t, 8192> continuity_{};
    bool audio_enabled_{};
    std::uint8_t pmt_version_{};
};

class MpegTsContinuityRewriter {
public:
    void reset();

    [[nodiscard]] bool rewrite_packets(
        std::uint8_t* data, std::size_t size);

    void append_discontinuity_packets(
        std::vector<std::uint8_t>& out) const;

private:
    std::array<std::uint8_t, 8192> next_continuity_{};
    std::array<bool, 8192> seen_{};
};

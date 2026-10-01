#pragma once

#include "cast_http_server.h"
#include "native_cast_stream.h"

#include <cstddef>
#include <cstdint>

class CastPipelineDiagnostics;

class NativeCastBackend {
public:
    explicit NativeCastBackend(
        std::size_t stream_capacity_bytes = 8U * 1024U * 1024U);
    ~NativeCastBackend();

    NativeCastBackend(const NativeCastBackend&) = delete;
    NativeCastBackend& operator=(const NativeCastBackend&) = delete;

    [[nodiscard]] bool start_http(std::uint16_t port);
    void stop();

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;

    void reset_stream();
    void set_diagnostics(CastPipelineDiagnostics* diagnostics) noexcept;

    void submit_h264_access_unit(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool keyframe);

    void submit_aac_adts_frame(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k);

private:
    NativeCastStream stream_;
    CastHttpServer http_server_;
};

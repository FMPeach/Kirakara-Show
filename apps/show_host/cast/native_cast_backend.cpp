#include "native_cast_backend.h"

NativeCastBackend::NativeCastBackend(std::size_t stream_capacity_bytes)
        : stream_(stream_capacity_bytes), http_server_(stream_) {}

NativeCastBackend::~NativeCastBackend() { stop(); }

bool NativeCastBackend::start_http(std::uint16_t port) {
    return http_server_.start(port);
}

void NativeCastBackend::stop() {
    http_server_.stop();
}

bool NativeCastBackend::is_running() const noexcept {
    return http_server_.is_running();
}

std::uint16_t NativeCastBackend::port() const noexcept {
    return http_server_.port();
}

void NativeCastBackend::reset_stream() {
    stream_.reset();
}

void NativeCastBackend::set_diagnostics(
        CastPipelineDiagnostics* diagnostics) noexcept {
    stream_.set_diagnostics(diagnostics);
}

void NativeCastBackend::submit_h264_access_unit(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k,
        bool keyframe) {
    stream_.append_h264_access_unit(data, size, pts90k, keyframe);
}

void NativeCastBackend::submit_aac_adts_frame(
        const std::uint8_t* data,
        std::size_t size,
        std::int64_t pts90k) {
    stream_.append_aac_adts_frame(data, size, pts90k);
}

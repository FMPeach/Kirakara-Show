#pragma once

#include <cstdint>
#include <memory>

class CastPipelineDiagnostics;

enum class D3D11CastFlushMode : std::uint32_t {
    unavailable = 0,
    full_context = 1,
    video_context = 2,
};

// GPU-only color conversion boundary for the native Cast encoder. The source
// is the renderer-owned fixed-canvas BGRA D3D11 texture; the destination is an
// NV12 D3D11 texture suitable for MFCreateDXGISurfaceBuffer. No pixels are
// captured from a window or mapped back to the CPU.
class D3D11BgraToNv12Converter {
public:
    D3D11BgraToNv12Converter();
    ~D3D11BgraToNv12Converter();

    D3D11BgraToNv12Converter(const D3D11BgraToNv12Converter&) = delete;
    D3D11BgraToNv12Converter& operator=(
        const D3D11BgraToNv12Converter&) = delete;

    [[nodiscard]] bool configure(
        void* d3d11_device,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t frame_rate_num,
        std::uint32_t frame_rate_den,
        CastPipelineDiagnostics* diagnostics = nullptr);
    void reset();

    // Returns an owned ID3D11Texture2D* through output_texture. The caller must
    // Release it. The texture remains entirely on the GPU.
    [[nodiscard]] bool create_output_texture(void** output_texture) const;

    // Cache video-processor views during Cast startup. Later convert calls for
    // these textures perform no resource or view creation.
    [[nodiscard]] bool prepare_input_texture(void* bgra_texture);
    [[nodiscard]] bool prepare_output_texture(void* nv12_texture);

    [[nodiscard]] bool convert(
        void* bgra_texture,
        void* nv12_texture,
        std::uint64_t correlation_id = 0);

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] D3D11CastFlushMode flush_mode() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

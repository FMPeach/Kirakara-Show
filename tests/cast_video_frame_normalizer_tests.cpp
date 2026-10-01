#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"
#include "../apps/show_host/cast/cast_video_frame_normalizer.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#include <d3d10.h>
#include <d3d11.h>
#include <mfobjects.h>

namespace {

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

MfD3D11VideoFormat format_for(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t par_num = 1,
        std::uint32_t par_den = 1) {
    MfD3D11VideoFormat format;
    format.pixel_format = MfD3D11VideoPixelFormat::nv12;
    format.width = width;
    format.height = height;
    format.pixel_aspect_ratio_num = par_num;
    format.pixel_aspect_ratio_den = par_den;
    format.frame_rate_num = 30;
    format.frame_rate_den = 1;
    format.interlace_mode = MFVideoInterlace_Progressive;
    format.video_primaries = MFVideoPrimaries_BT709;
    format.transfer_function = MFVideoTransFunc_709;
    format.yuv_matrix = MFVideoTransferMatrix_BT709;
    format.nominal_range = MFNominalRange_16_235;
    return format;
}

ID3D11Texture2D* create_nv12_texture(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        UINT bind_flags = D3D11_BIND_DECODER
            | D3D11_BIND_SHADER_RESOURCE) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind_flags;
    ID3D11Texture2D* texture{};
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
        return nullptr;
    }
    return texture;
}

void fill_nv12(
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture,
        std::uint32_t width,
        std::uint32_t height,
        std::uint8_t y,
        std::uint8_t u,
        std::uint8_t v) {
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 3 / 2, y);
    auto* uv = pixels.data() + static_cast<std::size_t>(width) * height;
    for (std::size_t index = 0;
            index < static_cast<std::size_t>(width) * height / 2;
            index += 2) {
        uv[index] = u;
        uv[index + 1] = v;
    }
    context->UpdateSubresource(
        texture, 0, nullptr, pixels.data(), width,
        static_cast<UINT>(pixels.size()));
}

std::uint8_t read_luma(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        ID3D11Texture2D* texture,
        std::uint32_t x,
        std::uint32_t y) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging_desc.MiscFlags = 0;
    ID3D11Texture2D* staging{};
    expect(SUCCEEDED(device->CreateTexture2D(
            &staging_desc, nullptr, &staging)) && staging,
        "create NV12 staging texture");
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    expect(SUCCEEDED(context->Map(
            staging, 0, D3D11_MAP_READ, 0, &mapped)),
        "map normalized NV12 texture");
    const auto value = static_cast<const std::uint8_t*>(mapped.pData)[
        static_cast<std::size_t>(y) * mapped.RowPitch + x];
    context->Unmap(staging, 0);
    release(staging);
    return value;
}

}  // namespace

int main() {
    const auto full = cast_calculate_video_normalize_geometry(
        format_for(1920, 1080), 1920, 1080);
    expect(full && full->direct_copy,
        "exact SDR 1080p should use the copy path");
    expect(full->source.width() == 1920 && full->source.height() == 1080,
        "exact source rect");
    expect(full->destination.width() == 1920
            && full->destination.height() == 1080,
        "exact destination rect");

    const auto downsample = cast_calculate_video_normalize_geometry(
        format_for(3840, 2160), 1920, 1080);
    expect(downsample && !downsample->direct_copy,
        "4K source should require one scale");
    expect(downsample->destination.left == 0
            && downsample->destination.top == 0
            && downsample->destination.right == 1920
            && downsample->destination.bottom == 1080,
        "16:9 4K should fill the output");

    for (const auto& format : {
            format_for(640, 360),
            format_for(854, 480),
            format_for(960, 540),
            format_for(1024, 576),
            format_for(1280, 720)}) {
        const auto geometry = cast_calculate_video_normalize_geometry(
            format, 1920, 1080);
        expect(geometry && geometry->destination.left == 0
                && geometry->destination.top == 0
                && geometry->destination.right == 1920
                && geometry->destination.bottom == 1080,
            "square-pixel 16:9 source should fill the output");
    }

    const auto ntsc = cast_calculate_video_normalize_geometry(
        format_for(720, 480, 8, 9), 1920, 1080);
    expect(ntsc && ntsc->destination.left == 240
            && ntsc->destination.right == 1680
            && ntsc->destination.top == 0
            && ntsc->destination.bottom == 1080,
        "720x480 8:9 should letterbox once as 4:3");
    const auto pal = cast_calculate_video_normalize_geometry(
        format_for(720, 576, 16, 15), 1920, 1080);
    expect(pal && pal->destination.left == 240
            && pal->destination.right == 1680,
        "720x576 16:15 should letterbox once as 4:3");

    auto cropped_format = format_for(1920, 1080);
    cropped_format.clean_aperture_x = 1.0;
    cropped_format.clean_aperture_width = 1918;
    cropped_format.clean_aperture_height = 1080;
    const auto cropped = cast_calculate_video_normalize_geometry(
        cropped_format, 1920, 1080);
    expect(cropped && !cropped->direct_copy,
        "odd clean aperture must not bypass normalization");
    expect((cropped->source.left & 1) == 0
            && (cropped->source.top & 1) == 0
            && (cropped->source.right & 1) == 0
            && (cropped->source.bottom & 1) == 0,
        "clean aperture source edges must be chroma aligned");
    expect((cropped->destination.left & 1) == 0
            && (cropped->destination.top & 1) == 0
            && (cropped->destination.right & 1) == 0
            && (cropped->destination.bottom & 1) == 0,
        "clean aperture destination edges must be chroma aligned");

    auto rotated_format = format_for(1080, 1920);
    rotated_format.rotation_degrees = 90;
    const auto rotated = cast_calculate_video_normalize_geometry(
        rotated_format, 1920, 1080);
    expect(rotated && !rotated->direct_copy
            && rotated->destination.left == 0
            && rotated->destination.top == 0
            && rotated->destination.right == 1920
            && rotated->destination.bottom == 1080,
        "90-degree portrait source should fill landscape after rotation");

    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    D3D_FEATURE_LEVEL level{};
    const auto hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &device, &level, &context);
    if (FAILED(hr) || !device || !context) {
        std::cout << "SKIP: no hardware D3D11 video device\n";
        release(context);
        release(device);
        return EXIT_SUCCESS;
    }
    ID3D10Multithread* multithread{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    CastPipelineDiagnostics diagnostics;
    CastVideoFrameNormalizer normalizer;
    CastVideoFrameNormalizerConfig config;
    config.surface_count = 4;
    config.create_plane_srvs = true;
    config.allow_direct_passthrough = true;
    expect(normalizer.configure(device, config, &diagnostics),
        "configure source-frame normalizer");

    auto invalid_config = config;
    invalid_config.create_plane_srvs = false;
    CastVideoFrameNormalizer invalid_normalizer;
    expect(!invalid_normalizer.configure(device, invalid_config),
        "direct passthrough requires NV12 plane SRVs");

    auto* exact_texture = create_nv12_texture(device, 1920, 1080);
    expect(exact_texture != nullptr, "create exact NV12 source texture");
    CastVideoFrameInput exact_input;
    exact_input.texture = exact_texture;
    exact_input.format = format_for(1920, 1080);
    exact_input.timestamp_seconds = 1.0;
    exact_input.duration_seconds = 1.0 / 30.0;
    exact_input.frame_id = 10;
    exact_input.generation = 2;
    exact_input.retention = std::make_shared<unsigned char>(0);
    auto exact_output = normalizer.normalize(exact_input);
    expect(exact_output && exact_output->texture(),
        "normalize exact NV12 source");
    expect(exact_output->texture() == exact_texture,
        "exact NV12 source should be borrowed without a second copy");
    expect(exact_output->y_srv() && exact_output->uv_srv(),
        "borrowed exact source exposes both NV12 plane views");
    expect(exact_output->retention_token() == exact_input.retention,
        "borrowed exact source forwards its decoder-pool retention token");
    expect(exact_output->format().width == 1920
            && exact_output->format().height == 1080,
        "normalizer publishes fixed output dimensions");
    expect(exact_output->format().yuv_matrix == MFVideoTransferMatrix_BT709
            && exact_output->format().nominal_range
                == MFNominalRange_16_235,
        "normalizer publishes fixed SDR colour metadata");
    const auto repeated = normalizer.normalize(exact_input);
    expect(repeated == exact_output,
        "repeated source frame should reuse cached normalized output");

    auto* no_srv_texture = create_nv12_texture(
        device, 1920, 1080, D3D11_BIND_DECODER);
    expect(no_srv_texture != nullptr,
        "create exact NV12 source without shader-resource binding");
    auto no_srv_input = exact_input;
    no_srv_input.texture = no_srv_texture;
    no_srv_input.frame_id = 11;
    expect(!normalizer.normalize(no_srv_input)
            && normalizer.last_failure()
                == CastVideoNormalizeFailure::plane_views_unavailable,
        "direct source without plane views must fail explicitly");
    release(no_srv_texture);

    auto hdr_input = exact_input;
    hdr_input.frame_id = 12;
    hdr_input.format.transfer_function = MFVideoTransFunc_2084;
    expect(!normalizer.normalize(hdr_input)
            && normalizer.last_failure()
                == CastVideoNormalizeFailure::hdr_unsupported,
        "HDR input must be rejected explicitly until tone mapping exists");

    auto* scaled_texture = create_nv12_texture(device, 640, 360);
    expect(scaled_texture != nullptr, "create scaled NV12 source texture");
    CastVideoFrameInput scaled_input;
    scaled_input.texture = scaled_texture;
    scaled_input.format = format_for(640, 360);
    scaled_input.timestamp_seconds = 2.0;
    scaled_input.duration_seconds = 1.0 / 30.0;
    scaled_input.frame_id = 13;
    scaled_input.generation = 2;
    auto scaled_output = normalizer.normalize(scaled_input);
    expect(scaled_output && scaled_output->texture(),
        "scale 360p NV12 source through one video-processor pass");

    auto* ntsc_texture = create_nv12_texture(device, 720, 480);
    expect(ntsc_texture != nullptr, "create NTSC NV12 source texture");
    fill_nv12(context, ntsc_texture, 720, 480, 235, 128, 128);
    CastVideoFrameInput ntsc_input;
    ntsc_input.texture = ntsc_texture;
    ntsc_input.format = format_for(720, 480, 8, 9);
    ntsc_input.timestamp_seconds = 3.0;
    ntsc_input.duration_seconds = 1.0 / 30.0;
    ntsc_input.frame_id = 14;
    ntsc_input.generation = 2;
    auto ntsc_output = normalizer.normalize(ntsc_input);
    expect(ntsc_output && ntsc_output->texture(),
        "normalize non-square-pixel NTSC source");
    const auto bar_luma = read_luma(device, context,
        static_cast<ID3D11Texture2D*>(ntsc_output->texture()), 100, 540);
    const auto picture_luma = read_luma(device, context,
        static_cast<ID3D11Texture2D*>(ntsc_output->texture()), 960, 540);
    expect(bar_luma < 40,
        "letterbox background is not video black");
    expect(picture_luma > 180,
        "letterbox center did not retain the bright source picture");

    const auto snapshot = diagnostics.snapshot();
    expect(snapshot.counter(CastPipelineCounter::gpu_copies) == 0,
        "exact source passthrough performs no normalizer GPU copy");
    expect(snapshot.counter(CastPipelineCounter::video_processor_passes) == 2,
        "scaled sources perform one video-processor pass each");
    expect(snapshot.counter(CastPipelineCounter::repeated_frames) == 1,
        "repeated source frame bypasses GPU normalization");
    expect(snapshot.event(CastPipelineEvent::normalize).count == 3,
        "only distinct accepted source frames emit normalize events");
    expect(snapshot.counter(CastPipelineCounter::explicit_flushes) == 0,
        "normalizer must not flush the immediate context per source frame");

    ntsc_output.reset();
    scaled_output.reset();
    exact_output.reset();
    normalizer.reset();
    release(ntsc_texture);
    release(scaled_texture);
    release(exact_texture);
    release(multithread);
    release(context);
    release(device);
    return EXIT_SUCCESS;
}

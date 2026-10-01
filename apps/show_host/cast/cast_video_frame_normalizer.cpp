#include "cast_video_frame_normalizer.h"

#include "cast_pipeline_diagnostics.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <d3d11_1.h>
#include <mfobjects.h>

namespace {

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool is_even_nonzero(std::uint32_t value) noexcept {
    return value != 0 && (value & 1U) == 0;
}

bool valid_rotation(std::uint32_t degrees) noexcept {
    return degrees == 0 || degrees == 90
        || degrees == 180 || degrees == 270;
}

bool progressive_or_unknown(std::uint32_t mode) noexcept {
    return mode == MFVideoInterlace_Unknown
        || mode == MFVideoInterlace_Progressive;
}

bool direct_sdr_colour(const MfD3D11VideoFormat& format) noexcept {
    const bool primaries = format.video_primaries == MFVideoPrimaries_Unknown
        || format.video_primaries == MFVideoPrimaries_BT709;
    const bool transfer = format.transfer_function == MFVideoTransFunc_Unknown
        || format.transfer_function == MFVideoTransFunc_709;
    const bool matrix = format.yuv_matrix == MFVideoTransferMatrix_Unknown
        || format.yuv_matrix == MFVideoTransferMatrix_BT709;
    const bool range = format.nominal_range == MFNominalRange_Unknown
        || format.nominal_range == MFNominalRange_16_235;
    return primaries && transfer && matrix && range;
}

bool explicit_hdr(const MfD3D11VideoFormat& format) noexcept {
    return format.transfer_function == MFVideoTransFunc_2084
        || format.transfer_function == MFVideoTransFunc_HLG;
}

std::int32_t even_floor(double value) noexcept {
    return static_cast<std::int32_t>(std::floor(value / 2.0)) * 2;
}

std::int32_t even_ceil(double value) noexcept {
    return static_cast<std::int32_t>(std::ceil(value / 2.0)) * 2;
}

std::int32_t nearest_even(double value) noexcept {
    return static_cast<std::int32_t>(std::llround(value / 2.0)) * 2;
}

D3D11_VIDEO_FRAME_FORMAT frame_format(std::uint32_t mode) noexcept {
    if (mode == MFVideoInterlace_FieldInterleavedUpperFirst
            || mode == MFVideoInterlace_FieldSingleUpper) {
        return D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
    }
    if (mode == MFVideoInterlace_FieldInterleavedLowerFirst
            || mode == MFVideoInterlace_FieldSingleLower) {
        return D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST;
    }
    return D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
}

D3D11_VIDEO_PROCESSOR_ROTATION rotation_value(
        std::uint32_t degrees) noexcept {
    switch (degrees) {
        case 90: return D3D11_VIDEO_PROCESSOR_ROTATION_90;
        case 180: return D3D11_VIDEO_PROCESSOR_ROTATION_180;
        case 270: return D3D11_VIDEO_PROCESSOR_ROTATION_270;
        default: return D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
    }
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE input_colour_space(
        const MfD3D11VideoFormat& format) noexcept {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE result{};
    result.YCbCr_Matrix = format.yuv_matrix == MFVideoTransferMatrix_BT601
        ? 0U : 1U;
    // Legacy D3D11 colour-space bits use 1 for studio range and 2 for full.
    result.Nominal_Range = format.nominal_range == MFNominalRange_0_255
        ? 2U : 1U;
    return result;
}

D3D11_VIDEO_PROCESSOR_COLOR_SPACE output_colour_space() noexcept {
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE result{};
    result.YCbCr_Matrix = 1;
    result.Nominal_Range = 1;
    return result;
}

bool same_processor_format(
        const MfD3D11VideoFormat& left,
        const MfD3D11VideoFormat& right) noexcept {
    return left.width == right.width
        && left.height == right.height
        && left.frame_rate_num == right.frame_rate_num
        && left.frame_rate_den == right.frame_rate_den
        && frame_format(left.interlace_mode)
            == frame_format(right.interlace_mode);
}

}  // namespace

std::optional<CastVideoNormalizeGeometry>
cast_calculate_video_normalize_geometry(
        const MfD3D11VideoFormat& format,
        std::uint32_t output_width,
        std::uint32_t output_height) noexcept {
    if (!format.valid()
            || format.pixel_format != MfD3D11VideoPixelFormat::nv12
            || !is_even_nonzero(format.width)
            || !is_even_nonzero(format.height)
            || !is_even_nonzero(output_width)
            || !is_even_nonzero(output_height)
            || !valid_rotation(format.rotation_degrees)) {
        return std::nullopt;
    }

    double clean_left = 0.0;
    double clean_top = 0.0;
    double clean_right = static_cast<double>(format.width);
    double clean_bottom = static_cast<double>(format.height);
    if (format.has_clean_aperture()) {
        clean_left = std::clamp(
            format.clean_aperture_x, 0.0,
            static_cast<double>(format.width));
        clean_top = std::clamp(
            format.clean_aperture_y, 0.0,
            static_cast<double>(format.height));
        clean_right = std::clamp(
            format.clean_aperture_x
                + static_cast<double>(format.clean_aperture_width),
            clean_left, static_cast<double>(format.width));
        clean_bottom = std::clamp(
            format.clean_aperture_y
                + static_cast<double>(format.clean_aperture_height),
            clean_top, static_cast<double>(format.height));
    }
    if (clean_right <= clean_left || clean_bottom <= clean_top) {
        return std::nullopt;
    }

    CastVideoNormalizeGeometry geometry;
    geometry.source.left = std::clamp(
        even_floor(clean_left), 0, static_cast<int>(format.width) - 2);
    geometry.source.top = std::clamp(
        even_floor(clean_top), 0, static_cast<int>(format.height) - 2);
    geometry.source.right = std::clamp(
        even_ceil(clean_right), geometry.source.left + 2,
        static_cast<int>(format.width));
    geometry.source.bottom = std::clamp(
        even_ceil(clean_bottom), geometry.source.top + 2,
        static_cast<int>(format.height));
    geometry.rotation_degrees = format.rotation_degrees;

    double display_width = (clean_right - clean_left)
        * static_cast<double>(format.pixel_aspect_ratio_num)
        / static_cast<double>(format.pixel_aspect_ratio_den);
    double display_height = clean_bottom - clean_top;
    if (format.rotation_degrees == 90 || format.rotation_degrees == 270) {
        std::swap(display_width, display_height);
    }
    if (!(display_width > 0.0) || !(display_height > 0.0)) {
        return std::nullopt;
    }

    const double source_aspect = display_width / display_height;
    const double output_aspect = static_cast<double>(output_width)
        / static_cast<double>(output_height);
    std::int32_t destination_width{};
    std::int32_t destination_height{};
    if (source_aspect >= output_aspect) {
        destination_width = static_cast<std::int32_t>(output_width);
        destination_height = nearest_even(
            static_cast<double>(output_width) / source_aspect);
    } else {
        destination_height = static_cast<std::int32_t>(output_height);
        destination_width = nearest_even(
            static_cast<double>(output_height) * source_aspect);
    }
    destination_width = std::clamp(
        destination_width, 2, static_cast<int>(output_width));
    destination_height = std::clamp(
        destination_height, 2, static_cast<int>(output_height));
    const auto destination_left = even_floor(
        (static_cast<double>(output_width) - destination_width) / 2.0);
    const auto destination_top = even_floor(
        (static_cast<double>(output_height) - destination_height) / 2.0);
    geometry.destination = {
        destination_left,
        destination_top,
        destination_left + destination_width,
        destination_top + destination_height,
    };

    geometry.direct_copy = geometry.source.left == 0
        && geometry.source.top == 0
        && geometry.source.right == static_cast<int>(format.width)
        && geometry.source.bottom == static_cast<int>(format.height)
        && geometry.destination.left == 0
        && geometry.destination.top == 0
        && geometry.destination.right == static_cast<int>(output_width)
        && geometry.destination.bottom == static_cast<int>(output_height)
        && format.width == output_width && format.height == output_height
        && clean_left == 0.0 && clean_top == 0.0
        && clean_right == static_cast<double>(format.width)
        && clean_bottom == static_cast<double>(format.height)
        && format.rotation_degrees == 0
        && format.pixel_aspect_ratio_num == format.pixel_aspect_ratio_den
        && progressive_or_unknown(format.interlace_mode)
        && direct_sdr_colour(format);
    return geometry;
}

CastNormalizedVideoFrame::CastNormalizedVideoFrame(
        CastNv12SurfacePool::Lease lease,
        MfD3D11VideoFormat format,
        double timestamp_seconds,
        double duration_seconds,
        std::uint64_t frame_id,
        std::uint64_t generation) noexcept
    : lease_(std::move(lease)),
      format_(format),
      timestamp_seconds_(timestamp_seconds),
      duration_seconds_(duration_seconds),
      frame_id_(frame_id),
      generation_(generation) {}

CastNormalizedVideoFrame::CastNormalizedVideoFrame(
        void* texture,
        void* y_srv,
        void* uv_srv,
        std::shared_ptr<void> retention,
        MfD3D11VideoFormat format,
        double timestamp_seconds,
        double duration_seconds,
        std::uint64_t frame_id,
        std::uint64_t generation) noexcept
    : borrowed_texture_(texture),
      borrowed_y_srv_(y_srv),
      borrowed_uv_srv_(uv_srv),
      borrowed_retention_(std::move(retention)),
      format_(format),
      timestamp_seconds_(timestamp_seconds),
      duration_seconds_(duration_seconds),
      frame_id_(frame_id),
      generation_(generation) {
    if (auto* value = static_cast<ID3D11Texture2D*>(borrowed_texture_)) {
        value->AddRef();
    }
    if (auto* value = static_cast<ID3D11ShaderResourceView*>(borrowed_y_srv_)) {
        value->AddRef();
    }
    if (auto* value = static_cast<ID3D11ShaderResourceView*>(borrowed_uv_srv_)) {
        value->AddRef();
    }
}

CastNormalizedVideoFrame::~CastNormalizedVideoFrame() {
    auto* uv = static_cast<ID3D11ShaderResourceView*>(borrowed_uv_srv_);
    auto* y = static_cast<ID3D11ShaderResourceView*>(borrowed_y_srv_);
    auto* texture = static_cast<ID3D11Texture2D*>(borrowed_texture_);
    release(uv);
    release(y);
    release(texture);
    borrowed_uv_srv_ = nullptr;
    borrowed_y_srv_ = nullptr;
    borrowed_texture_ = nullptr;
}

void* CastNormalizedVideoFrame::texture() const noexcept {
    return borrowed_texture_ ? borrowed_texture_ : lease_.compose_texture();
}
void* CastNormalizedVideoFrame::y_srv() const noexcept {
    return borrowed_y_srv_ ? borrowed_y_srv_ : lease_.y_srv();
}
void* CastNormalizedVideoFrame::uv_srv() const noexcept {
    return borrowed_uv_srv_ ? borrowed_uv_srv_ : lease_.uv_srv();
}
std::shared_ptr<void>
CastNormalizedVideoFrame::retention_token() const noexcept {
    return borrowed_texture_
        ? borrowed_retention_ : lease_.retention_token();
}
const MfD3D11VideoFormat&
CastNormalizedVideoFrame::format() const noexcept {
    return format_;
}
double CastNormalizedVideoFrame::timestamp_seconds() const noexcept {
    return timestamp_seconds_;
}
double CastNormalizedVideoFrame::duration_seconds() const noexcept {
    return duration_seconds_;
}
std::uint64_t CastNormalizedVideoFrame::frame_id() const noexcept {
    return frame_id_;
}
std::uint64_t CastNormalizedVideoFrame::generation() const noexcept {
    return generation_;
}

struct CastVideoFrameNormalizer::Impl {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11VideoDevice* video_device{};
    ID3D11VideoContext* video_context{};
    ID3D11VideoContext1* video_context1{};
    ID3D11VideoProcessorEnumerator* enumerator{};
    ID3D11VideoProcessor* processor{};
    std::unordered_map<ID3D11Texture2D*, ID3D11VideoProcessorInputView*>
        input_views;
    struct PlaneViews {
        ID3D11ShaderResourceView* y{};
        ID3D11ShaderResourceView* uv{};
    };
    std::unordered_map<ID3D11Texture2D*, PlaneViews> plane_views;
    std::vector<ID3D11VideoProcessorOutputView*> output_views;
    CastNv12SurfacePool surfaces;
    CastVideoFrameNormalizerConfig config;
    MfD3D11VideoFormat processor_format;
    std::uint64_t view_generation{};
    std::uint64_t plane_view_generation{};
    bool processor_configured{};
    CastPipelineDiagnostics* diagnostics{};
    CastVideoNormalizeFailure failure{CastVideoNormalizeFailure::none};
    HRESULT failure_hr{S_OK};
    void* cached_source_texture{};
    std::uint64_t cached_frame_id{};
    std::uint64_t cached_generation{};
    double cached_timestamp{};
    std::shared_ptr<const CastNormalizedVideoFrame> cached_frame;

    ~Impl() { reset(); }

    void clear_views() noexcept {
        for (auto& [texture, view] : input_views) {
            static_cast<void>(texture);
            release(view);
        }
        input_views.clear();
        for (auto& [texture, views] : plane_views) {
            static_cast<void>(texture);
            release(views.uv);
            release(views.y);
        }
        plane_views.clear();
        for (auto*& view : output_views) release(view);
        output_views.clear();
    }

    void clear_processor() noexcept {
        clear_views();
        release(processor);
        release(enumerator);
        processor_format = {};
        processor_configured = false;
        view_generation = 0;
        plane_view_generation = 0;
    }

    void reset() noexcept {
        cached_frame.reset();
        cached_source_texture = nullptr;
        cached_frame_id = 0;
        cached_generation = 0;
        cached_timestamp = 0.0;
        clear_processor();
        surfaces.reset();
        release(video_context1);
        release(video_context);
        release(video_device);
        release(context);
        release(device);
        config = {};
        diagnostics = nullptr;
        failure = CastVideoNormalizeFailure::none;
        failure_hr = S_OK;
    }

    bool configure(
            ID3D11Device* requested_device,
            const CastVideoFrameNormalizerConfig& requested_config,
            CastPipelineDiagnostics* requested_diagnostics) {
        reset();
        if (!requested_device
                || !is_even_nonzero(requested_config.width)
                || !is_even_nonzero(requested_config.height)
                || requested_config.frame_rate_num == 0
                || requested_config.frame_rate_den == 0
                || requested_config.surface_count < 2
                || (requested_config.allow_direct_passthrough
                    && !requested_config.create_plane_srvs)) {
            failure = CastVideoNormalizeFailure::not_configured;
            failure_hr = E_INVALIDARG;
            return false;
        }
        requested_device->AddRef();
        device = requested_device;
        device->GetImmediateContext(&context);
        if (!context) {
            reset();
            failure = CastVideoNormalizeFailure::not_configured;
            failure_hr = E_FAIL;
            return false;
        }

        config = requested_config;
        diagnostics = requested_diagnostics;
        CastNv12SurfacePoolConfig pool_config;
        pool_config.width = config.width;
        pool_config.height = config.height;
        pool_config.capacity = config.surface_count;
        pool_config.create_plane_srvs = config.create_plane_srvs;
        pool_config.create_plane_uavs = false;
        pool_config.video_processor_output = true;
        pool_config.encoder_compatible = false;
        if (!surfaces.configure(device, pool_config, diagnostics)) {
            reset();
            failure = CastVideoNormalizeFailure::not_configured;
            failure_hr = E_FAIL;
            return false;
        }
        failure = CastVideoNormalizeFailure::none;
        failure_hr = S_OK;
        return true;
    }

    bool configure_processor_for(const MfD3D11VideoFormat& format) {
        if (processor_configured
                && same_processor_format(processor_format, format)) {
            return true;
        }
        clear_processor();
        if (!video_device) {
            static_cast<void>(device->QueryInterface(
                IID_PPV_ARGS(&video_device)));
        }
        if (!video_context) {
            static_cast<void>(context->QueryInterface(
                IID_PPV_ARGS(&video_context)));
        }
        if (!video_context1 && video_context) {
            static_cast<void>(video_context->QueryInterface(
                IID_PPV_ARGS(&video_context1)));
        }
        if (!video_device || !video_context) return false;
        if (format.rotation_degrees != 0 && !video_context1) {
            failure = CastVideoNormalizeFailure::rotation_unsupported;
            failure_hr = E_NOINTERFACE;
            return false;
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = frame_format(format.interlace_mode);
        content.InputFrameRate.Numerator = format.frame_rate_num != 0
            ? format.frame_rate_num : config.frame_rate_num;
        content.InputFrameRate.Denominator = format.frame_rate_den != 0
            ? format.frame_rate_den : config.frame_rate_den;
        content.InputWidth = format.width;
        content.InputHeight = format.height;
        content.OutputFrameRate.Numerator = config.frame_rate_num;
        content.OutputFrameRate.Denominator = config.frame_rate_den;
        content.OutputWidth = config.width;
        content.OutputHeight = config.height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        auto hr = video_device->CreateVideoProcessorEnumerator(
            &content, &enumerator);
        UINT input_support{};
        UINT output_support{};
        if (SUCCEEDED(hr)) {
            hr = enumerator->CheckVideoProcessorFormat(
                DXGI_FORMAT_NV12, &input_support);
        }
        if (SUCCEEDED(hr)) {
            hr = enumerator->CheckVideoProcessorFormat(
                DXGI_FORMAT_NV12, &output_support);
        }
        if (SUCCEEDED(hr)
                && ((input_support
                        & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0
                    || (output_support
                        & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)) {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            hr = video_device->CreateVideoProcessor(
                enumerator, 0, &processor);
        }
        if (FAILED(hr) || !processor) {
            failure_hr = FAILED(hr) ? hr : E_FAIL;
            clear_processor();
            return false;
        }

        output_views.resize(surfaces.capacity());
        for (std::size_t index = 0; index < output_views.size(); ++index) {
            auto* texture = static_cast<ID3D11Texture2D*>(
                surfaces.compose_texture_at(index));
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};
            view.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipSlice = 0;
            hr = video_device->CreateVideoProcessorOutputView(
                texture, enumerator, &view, &output_views[index]);
            if (FAILED(hr) || !output_views[index]) {
                failure_hr = FAILED(hr) ? hr : E_FAIL;
                clear_processor();
                return false;
            }
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::view_creations);
            }
        }
        processor_format = format;
        processor_configured = true;
        return true;
    }

    ID3D11VideoProcessorInputView* input_view_for(
            ID3D11Texture2D* texture,
            std::uint64_t generation) {
        if (view_generation != generation) {
            for (auto& [entry_texture, view] : input_views) {
                static_cast<void>(entry_texture);
                release(view);
            }
            input_views.clear();
            view_generation = generation;
        }
        const auto found = input_views.find(texture);
        if (found != input_views.end()) return found->second;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};
        view.FourCC = 0;
        view.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = 0;
        view.Texture2D.ArraySlice = 0;
        ID3D11VideoProcessorInputView* result{};
        const auto hr = video_device->CreateVideoProcessorInputView(
            texture, enumerator, &view, &result);
        if (FAILED(hr) || !result) {
            failure_hr = FAILED(hr) ? hr : E_FAIL;
            return nullptr;
        }
        try {
            const auto [entry, inserted] = input_views.emplace(texture, result);
            if (!inserted) {
                release(result);
                return entry->second;
            }
        } catch (...) {
            release(result);
            return nullptr;
        }
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::view_creations);
        }
        return result;
    }

    const PlaneViews* plane_views_for(
            ID3D11Texture2D* texture,
            std::uint64_t generation) {
        if (plane_view_generation != generation) {
            for (auto& [entry_texture, views] : plane_views) {
                static_cast<void>(entry_texture);
                release(views.uv);
                release(views.y);
            }
            plane_views.clear();
            plane_view_generation = generation;
        }
        const auto found = plane_views.find(texture);
        if (found != plane_views.end()) return &found->second;

        PlaneViews candidate;
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = DXGI_FORMAT_R8_UNORM;
        auto hr = device->CreateShaderResourceView(
            texture, &view, &candidate.y);
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(
                texture, &view, &candidate.uv);
        }
        if (FAILED(hr) || !candidate.y || !candidate.uv) {
            release(candidate.uv);
            release(candidate.y);
            failure = CastVideoNormalizeFailure::plane_views_unavailable;
            failure_hr = FAILED(hr) ? hr : E_FAIL;
            return nullptr;
        }
        try {
            const auto [entry, inserted] = plane_views.emplace(
                texture, candidate);
            if (!inserted) {
                release(candidate.uv);
                release(candidate.y);
            } else if (diagnostics) {
                diagnostics->increment(
                    CastPipelineCounter::view_creations, 2);
            }
            return &entry->second;
        } catch (...) {
            release(candidate.uv);
            release(candidate.y);
            failure = CastVideoNormalizeFailure::surface_unavailable;
            failure_hr = E_OUTOFMEMORY;
            return nullptr;
        }
    }

    bool video_process(
            ID3D11Texture2D* source,
            CastNv12SurfacePool::Lease& destination,
            const MfD3D11VideoFormat& format,
            const CastVideoNormalizeGeometry& geometry,
            std::uint64_t generation) {
        if (!configure_processor_for(format)) {
            if (failure == CastVideoNormalizeFailure::none) {
                failure = CastVideoNormalizeFailure::video_processor_unavailable;
            }
            return false;
        }
        auto* input_view = input_view_for(source, generation);
        const auto index = destination.slot_index();
        if (!input_view || index >= output_views.size()
                || !output_views[index]) {
            failure = CastVideoNormalizeFailure::video_processor_unavailable;
            if (SUCCEEDED(failure_hr)) failure_hr = E_FAIL;
            return false;
        }

        RECT output_rect{0, 0,
            static_cast<LONG>(config.width),
            static_cast<LONG>(config.height)};
        RECT source_rect{
            geometry.source.left, geometry.source.top,
            geometry.source.right, geometry.source.bottom};
        RECT destination_rect{
            geometry.destination.left, geometry.destination.top,
            geometry.destination.right, geometry.destination.bottom};
        D3D11_VIDEO_COLOR background{};
        background.YCbCr.Y = 0.0F;
        background.YCbCr.Cb = 0.5F;
        background.YCbCr.Cr = 0.5F;
        background.YCbCr.A = 1.0F;
        const auto input_space = input_colour_space(format);
        const auto output_space = output_colour_space();
        video_context->VideoProcessorSetOutputTargetRect(
            processor, TRUE, &output_rect);
        video_context->VideoProcessorSetOutputBackgroundColor(
            processor, TRUE, &background);
        video_context->VideoProcessorSetOutputColorSpace(
            processor, &output_space);
        video_context->VideoProcessorSetStreamFrameFormat(
            processor, 0, frame_format(format.interlace_mode));
        video_context->VideoProcessorSetStreamColorSpace(
            processor, 0, &input_space);
        video_context->VideoProcessorSetStreamSourceRect(
            processor, 0, TRUE, &source_rect);
        video_context->VideoProcessorSetStreamDestRect(
            processor, 0, TRUE, &destination_rect);
        video_context->VideoProcessorSetStreamAutoProcessingMode(
            processor, 0, progressive_or_unknown(format.interlace_mode)
                ? FALSE : TRUE);
        if (video_context1) {
            video_context1->VideoProcessorSetStreamRotation(
                processor,
                0,
                format.rotation_degrees != 0,
                rotation_value(format.rotation_degrees));
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view;
        const auto hr = video_context->VideoProcessorBlt(
            processor, output_views[index], 0, 1, &stream);
        if (FAILED(hr)) {
            failure = CastVideoNormalizeFailure::video_processor_failed;
            failure_hr = hr;
            return false;
        }
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::video_processor_passes);
        }
        return true;
    }

    std::shared_ptr<const CastNormalizedVideoFrame> normalize(
            const CastVideoFrameInput& input) {
        failure = CastVideoNormalizeFailure::none;
        failure_hr = S_OK;
        if (!device || !context) {
            failure = CastVideoNormalizeFailure::not_configured;
            failure_hr = E_UNEXPECTED;
            return {};
        }
        if (!input.texture || !input.format.valid()
                || input.format.pixel_format
                    != MfD3D11VideoPixelFormat::nv12) {
            failure = CastVideoNormalizeFailure::invalid_frame;
            failure_hr = E_INVALIDARG;
            return {};
        }
        if (explicit_hdr(input.format)) {
            failure = CastVideoNormalizeFailure::hdr_unsupported;
            failure_hr = E_NOTIMPL;
            return {};
        }
        const auto geometry = cast_calculate_video_normalize_geometry(
            input.format, config.width, config.height);
        if (!geometry) {
            failure = CastVideoNormalizeFailure::invalid_frame;
            failure_hr = E_INVALIDARG;
            return {};
        }
        if (cached_frame
                && cached_source_texture == input.texture
                && cached_frame_id == input.frame_id
                && cached_generation == input.generation
                && cached_timestamp == input.timestamp_seconds) {
            if (diagnostics) {
                diagnostics->increment(CastPipelineCounter::repeated_frames);
            }
            return cached_frame;
        }

        auto* source = static_cast<ID3D11Texture2D*>(input.texture);
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_desc.Format != DXGI_FORMAT_NV12
                || source_desc.Width != input.format.width
                || source_desc.Height != input.format.height) {
            failure = CastVideoNormalizeFailure::invalid_frame;
            failure_hr = E_INVALIDARG;
            return {};
        }
        ID3D11Device* source_device{};
        source->GetDevice(&source_device);
        const bool same_device = source_device == device;
        release(source_device);
        if (!same_device) {
            failure = CastVideoNormalizeFailure::device_mismatch;
            failure_hr = E_INVALIDARG;
            return {};
        }

        const auto started = diagnostics
            ? CastPipelineDiagnostics::qpc_now() : 0;
        MfD3D11VideoFormat output_format;
        output_format.pixel_format = MfD3D11VideoPixelFormat::nv12;
        output_format.width = config.width;
        output_format.height = config.height;
        output_format.pixel_aspect_ratio_num = 1;
        output_format.pixel_aspect_ratio_den = 1;
        output_format.frame_rate_num = config.frame_rate_num;
        output_format.frame_rate_den = config.frame_rate_den;
        output_format.interlace_mode = MFVideoInterlace_Progressive;
        output_format.video_primaries = MFVideoPrimaries_BT709;
        output_format.transfer_function = MFVideoTransFunc_709;
        output_format.yuv_matrix = MFVideoTransferMatrix_BT709;
        output_format.nominal_range = MFNominalRange_16_235;

        std::shared_ptr<const CastNormalizedVideoFrame> normalized;
        std::int64_t normalize_kind{};
        if (geometry->direct_copy && config.allow_direct_passthrough) {
            const auto* views = plane_views_for(source, input.generation);
            if (!views) return {};
            try {
                normalized = std::make_shared<CastNormalizedVideoFrame>(
                    source,
                    views->y,
                    views->uv,
                    input.retention,
                    output_format,
                    input.timestamp_seconds,
                    input.duration_seconds,
                    input.frame_id,
                    input.generation);
            } catch (...) {
                failure = CastVideoNormalizeFailure::surface_unavailable;
                failure_hr = E_OUTOFMEMORY;
                return {};
            }
        } else {
            auto lease = surfaces.try_acquire();
            if (!lease) {
                failure = CastVideoNormalizeFailure::surface_unavailable;
                failure_hr = DXGI_ERROR_WAS_STILL_DRAWING;
                return {};
            }
            if (geometry->direct_copy) {
                auto* destination = static_cast<ID3D11Texture2D*>(
                    lease.compose_texture());
                context->CopyResource(destination, source);
                if (diagnostics) {
                    diagnostics->increment(CastPipelineCounter::gpu_copies);
                }
                normalize_kind = 1;
            } else if (!video_process(
                    source, lease, input.format, *geometry,
                    input.generation)) {
                return {};
            } else {
                normalize_kind = 2;
            }
            try {
                normalized = std::make_shared<CastNormalizedVideoFrame>(
                    std::move(lease),
                    output_format,
                    input.timestamp_seconds,
                    input.duration_seconds,
                    input.frame_id,
                    input.generation);
            } catch (...) {
                failure = CastVideoNormalizeFailure::surface_unavailable;
                failure_hr = E_OUTOFMEMORY;
                return {};
            }
        }
        if (diagnostics) {
            diagnostics->record_duration(
                CastPipelineEvent::normalize,
                input.frame_id,
                started,
                CastPipelineDiagnostics::qpc_now(),
                input.generation,
                normalize_kind);
        }
        cached_source_texture = input.texture;
        cached_frame_id = input.frame_id;
        cached_generation = input.generation;
        cached_timestamp = input.timestamp_seconds;
        cached_frame = normalized;
        return normalized;
    }
};

CastVideoFrameNormalizer::CastVideoFrameNormalizer()
    : impl_(std::make_unique<Impl>()) {}

CastVideoFrameNormalizer::~CastVideoFrameNormalizer() = default;

bool CastVideoFrameNormalizer::configure(
        void* d3d11_device,
        const CastVideoFrameNormalizerConfig& config,
        CastPipelineDiagnostics* diagnostics) {
    return impl_ && impl_->configure(
        static_cast<ID3D11Device*>(d3d11_device), config, diagnostics);
}

void CastVideoFrameNormalizer::reset() noexcept {
    if (impl_) impl_->reset();
}

std::shared_ptr<const CastNormalizedVideoFrame>
CastVideoFrameNormalizer::normalize(const CastVideoFrameInput& input) {
    return impl_ ? impl_->normalize(input) : nullptr;
}

CastVideoNormalizeFailure
CastVideoFrameNormalizer::last_failure() const noexcept {
    return impl_ ? impl_->failure
        : CastVideoNormalizeFailure::not_configured;
}

std::int32_t CastVideoFrameNormalizer::last_hresult() const noexcept {
    return impl_ ? static_cast<std::int32_t>(impl_->failure_hr)
        : static_cast<std::int32_t>(E_UNEXPECTED);
}

std::size_t CastVideoFrameNormalizer::available_surfaces() const noexcept {
    return impl_ ? impl_->surfaces.available() : 0;
}

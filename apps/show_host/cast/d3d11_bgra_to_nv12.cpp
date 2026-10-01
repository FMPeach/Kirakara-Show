#include "d3d11_bgra_to_nv12.h"

#include "cast_pipeline_diagnostics.h"

#include <algorithm>
#include <unordered_map>

#include <d3d11.h>
#include <d3d11_3.h>

namespace {

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

}  // namespace

struct D3D11BgraToNv12Converter::Impl {
    ID3D11Device* device{};
    ID3D11DeviceContext* context{};
    ID3D11DeviceContext3* context3{};
    ID3D11VideoDevice* video_device{};
    ID3D11VideoContext* video_context{};
    ID3D11VideoProcessorEnumerator* enumerator{};
    ID3D11VideoProcessor* processor{};
    std::unordered_map<ID3D11Texture2D*, ID3D11VideoProcessorInputView*>
        input_views;
    std::unordered_map<ID3D11Texture2D*, ID3D11VideoProcessorOutputView*>
        output_views;
    std::uint32_t width{};
    std::uint32_t height{};
    CastPipelineDiagnostics* diagnostics{};

    ~Impl() { reset(); }

    void reset() {
        for (auto& [texture, view] : output_views) {
            static_cast<void>(texture);
            release(view);
        }
        output_views.clear();
        for (auto& [texture, view] : input_views) {
            static_cast<void>(texture);
            release(view);
        }
        input_views.clear();
        release(processor);
        release(enumerator);
        release(video_context);
        release(video_device);
        release(context3);
        release(context);
        release(device);
        width = 0;
        height = 0;
        diagnostics = nullptr;
    }

    bool configure(
            ID3D11Device* requested_device,
            std::uint32_t requested_width,
            std::uint32_t requested_height,
            std::uint32_t frame_rate_num,
            std::uint32_t frame_rate_den,
            CastPipelineDiagnostics* requested_diagnostics) {
        reset();
        if (!requested_device || requested_width == 0 || requested_height == 0
                || (requested_width & 1U) != 0
                || (requested_height & 1U) != 0
                || frame_rate_num == 0 || frame_rate_den == 0) {
            return false;
        }

        requested_device->AddRef();
        device = requested_device;
        device->GetImmediateContext(&context);
        if (context) {
            // D3D11.3 can submit only queued video commands. Older runtimes
            // keep the existing full-context flush; the choice is fixed here
            // and never reprobed in the per-frame path.
            static_cast<void>(context->QueryInterface(
                IID_PPV_ARGS(&context3)));
        }
        auto hr = device->QueryInterface(IID_PPV_ARGS(&video_device));
        if (SUCCEEDED(hr) && context) {
            hr = context->QueryInterface(IID_PPV_ARGS(&video_context));
        }

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputFrameRate.Numerator = frame_rate_num;
        content.InputFrameRate.Denominator = frame_rate_den;
        content.InputWidth = requested_width;
        content.InputHeight = requested_height;
        content.OutputFrameRate.Numerator = frame_rate_num;
        content.OutputFrameRate.Denominator = frame_rate_den;
        content.OutputWidth = requested_width;
        content.OutputHeight = requested_height;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (SUCCEEDED(hr)) {
            hr = video_device->CreateVideoProcessorEnumerator(
                &content, &enumerator);
        }

        UINT bgra_support{};
        UINT nv12_support{};
        if (SUCCEEDED(hr)) {
            hr = enumerator->CheckVideoProcessorFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM, &bgra_support);
        }
        if (SUCCEEDED(hr)) {
            hr = enumerator->CheckVideoProcessorFormat(
                DXGI_FORMAT_NV12, &nv12_support);
        }
        if (SUCCEEDED(hr)
                && (bgra_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
                    == 0) {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)
                && (nv12_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)
                    == 0) {
            hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) {
            hr = video_device->CreateVideoProcessor(
                enumerator, 0, &processor);
        }
        if (FAILED(hr)) {
            reset();
            return false;
        }

        width = requested_width;
        height = requested_height;
        diagnostics = requested_diagnostics;
        return true;
    }

    bool create_output_texture(ID3D11Texture2D** output) const {
        if (!output) return false;
        *output = nullptr;
        if (!device || width == 0 || height == 0) return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // The video processor writes NV12 into this texture. Hardware MFTs
        // that support DXGI input consume it directly; compatibility paths
        // can still read it back for system-memory input.
        desc.BindFlags = D3D11_BIND_RENDER_TARGET
            | D3D11_BIND_VIDEO_ENCODER;
        const auto created = SUCCEEDED(
            device->CreateTexture2D(&desc, nullptr, output));
        if (created && diagnostics) {
            diagnostics->increment(CastPipelineCounter::texture_creations);
        }
        return created;
    }

    bool prepare_input_texture(ID3D11Texture2D* source) {
        if (!source || !video_device || !enumerator) return false;
        if (input_views.find(source) != input_views.end()) return true;

        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        if (source_desc.Width != width || source_desc.Height != height
                || source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            return false;
        }

        ID3D11Device* source_device{};
        source->GetDevice(&source_device);
        const bool same_device = source_device == device;
        release(source_device);
        if (!same_device) return false;

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.FourCC = 0;
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_desc.Texture2D.MipSlice = 0;
        input_desc.Texture2D.ArraySlice = 0;
        ID3D11VideoProcessorInputView* input_view{};
        const auto hr = video_device->CreateVideoProcessorInputView(
            source, enumerator, &input_desc, &input_view);
        if (FAILED(hr) || !input_view) return false;
        try {
            const auto [entry, inserted] = input_views.emplace(
                source, input_view);
            static_cast<void>(entry);
            if (!inserted) {
                release(input_view);
                return true;
            }
        } catch (...) {
            release(input_view);
            return false;
        }
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::view_creations);
        }
        return true;
    }

    bool prepare_output_texture(ID3D11Texture2D* destination) {
        if (!destination || !video_device || !enumerator) return false;
        if (output_views.find(destination) != output_views.end()) return true;

        D3D11_TEXTURE2D_DESC destination_desc{};
        destination->GetDesc(&destination_desc);
        if (destination_desc.Width != width
                || destination_desc.Height != height
                || destination_desc.Format != DXGI_FORMAT_NV12) {
            return false;
        }

        ID3D11Device* destination_device{};
        destination->GetDevice(&destination_device);
        const bool same_device = destination_device == device;
        release(destination_device);
        if (!same_device) return false;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
        output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        output_desc.Texture2D.MipSlice = 0;
        ID3D11VideoProcessorOutputView* output_view{};
        const auto hr = video_device->CreateVideoProcessorOutputView(
            destination, enumerator, &output_desc, &output_view);
        if (FAILED(hr) || !output_view) return false;
        try {
            const auto [entry, inserted] = output_views.emplace(
                destination, output_view);
            static_cast<void>(entry);
            if (!inserted) {
                release(output_view);
                return true;
            }
        } catch (...) {
            release(output_view);
            return false;
        }
        if (diagnostics) {
            diagnostics->increment(CastPipelineCounter::view_creations);
        }
        return true;
    }

    bool convert(
            ID3D11Texture2D* source,
            ID3D11Texture2D* destination,
            std::uint64_t correlation_id) {
        if (!source || !destination || !video_device || !video_context
                || !enumerator || !processor) {
            return false;
        }
        D3D11_TEXTURE2D_DESC source_desc{};
        D3D11_TEXTURE2D_DESC destination_desc{};
        source->GetDesc(&source_desc);
        destination->GetDesc(&destination_desc);
        if (source_desc.Width != width || source_desc.Height != height
                || source_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM
                || destination_desc.Width != width
                || destination_desc.Height != height
                || destination_desc.Format != DXGI_FORMAT_NV12) {
            return false;
        }

        if (!prepare_input_texture(source)
                || !prepare_output_texture(destination)) {
            return false;
        }
        auto* input_view = input_views.find(source)->second;
        auto* output_view = output_views.find(destination)->second;

        RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        const auto compose_started = diagnostics
            ? CastPipelineDiagnostics::qpc_now() : 0;
        video_context->VideoProcessorSetOutputTargetRect(
            processor, TRUE, &rect);
        video_context->VideoProcessorSetStreamFrameFormat(
            processor, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        video_context->VideoProcessorSetStreamSourceRect(
            processor, 0, TRUE, &rect);
        video_context->VideoProcessorSetStreamDestRect(
            processor, 0, TRUE, &rect);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input_view;
        const auto hr = video_context->VideoProcessorBlt(
            processor, output_view, 0, 1, &stream);
        if (SUCCEEDED(hr)) {
            if (diagnostics) {
                diagnostics->increment(
                    CastPipelineCounter::video_processor_passes);
            }
            const auto mode = context3
                ? D3D11CastFlushMode::video_context
                : D3D11CastFlushMode::full_context;
            if (context3) {
                context3->Flush1(D3D11_CONTEXT_TYPE_VIDEO, nullptr);
            } else {
                context->Flush();
            }
            if (diagnostics) {
                diagnostics->increment(
                    CastPipelineCounter::explicit_flushes);
                diagnostics->record_duration(
                    CastPipelineEvent::nv12_compose,
                    correlation_id,
                    compose_started,
                    CastPipelineDiagnostics::qpc_now(),
                    0,
                    static_cast<std::int64_t>(mode));
            }
        }

        return SUCCEEDED(hr);
    }
};

D3D11BgraToNv12Converter::D3D11BgraToNv12Converter()
        : impl_(std::make_unique<Impl>()) {}

D3D11BgraToNv12Converter::~D3D11BgraToNv12Converter() = default;

bool D3D11BgraToNv12Converter::configure(
        void* d3d11_device,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t frame_rate_num,
        std::uint32_t frame_rate_den,
        CastPipelineDiagnostics* diagnostics) {
    return impl_ && impl_->configure(
        static_cast<ID3D11Device*>(d3d11_device),
        width,
        height,
        frame_rate_num,
        frame_rate_den,
        diagnostics);
}

void D3D11BgraToNv12Converter::reset() {
    if (impl_) impl_->reset();
}

bool D3D11BgraToNv12Converter::prepare_input_texture(
        void* bgra_texture) {
    return impl_ && impl_->prepare_input_texture(
        static_cast<ID3D11Texture2D*>(bgra_texture));
}

bool D3D11BgraToNv12Converter::prepare_output_texture(
        void* nv12_texture) {
    return impl_ && impl_->prepare_output_texture(
        static_cast<ID3D11Texture2D*>(nv12_texture));
}

bool D3D11BgraToNv12Converter::create_output_texture(
        void** output_texture) const {
    if (!output_texture) return false;
    *output_texture = nullptr;
    ID3D11Texture2D* texture{};
    if (!impl_ || !impl_->create_output_texture(&texture)) return false;
    *output_texture = texture;
    return true;
}

bool D3D11BgraToNv12Converter::convert(
        void* bgra_texture,
        void* nv12_texture,
        std::uint64_t correlation_id) {
    return impl_ && impl_->convert(
        static_cast<ID3D11Texture2D*>(bgra_texture),
        static_cast<ID3D11Texture2D*>(nv12_texture),
        correlation_id);
}

std::uint32_t D3D11BgraToNv12Converter::width() const noexcept {
    return impl_ ? impl_->width : 0;
}

std::uint32_t D3D11BgraToNv12Converter::height() const noexcept {
    return impl_ ? impl_->height : 0;
}

D3D11CastFlushMode D3D11BgraToNv12Converter::flush_mode() const noexcept {
    if (!impl_ || !impl_->context) return D3D11CastFlushMode::unavailable;
    return impl_->context3
        ? D3D11CastFlushMode::video_context
        : D3D11CastFlushMode::full_context;
}

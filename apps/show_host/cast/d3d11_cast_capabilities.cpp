#include "d3d11_cast_capabilities.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <new>
#include <string>
#include <thread>

#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <shlwapi.h>
#include <windows.h>
#include <winternl.h>

#ifndef __IMFTrackedSample_INTERFACE_DEFINED__
struct IMFTrackedSample : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetAllocator(
        IMFAsyncCallback* sample_allocator,
        IUnknown* state) = 0;
};
#endif

extern "C" HRESULT WINAPI MFCreateTrackedSample(IMFTrackedSample** sample);

namespace {

// MinGW-w64 8 omits these declarations even though the APIs are available on
// every Windows version supported by Show. Keep private equivalents so the
// capability probe stays buildable with both the production MSYS2 toolchain
// and the older cross-check toolchain used in CI.
constexpr GUID kReadwriteEnableHardwareTransforms{
    0xa634a91c, 0x822b, 0x41b9,
    {0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0}};
constexpr HRESULT kMfNoEventsAvailable = static_cast<HRESULT>(
    static_cast<std::int32_t>(0xc00d3e80U));
constexpr HRESULT kMfEndOfStream = static_cast<HRESULT>(
    static_cast<std::int32_t>(0xc00d3e84U));

template <typename T>
void release(T*& value) noexcept {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

class TrackedReleaseCallback final : public IMFAsyncCallback {
public:
    TrackedReleaseCallback() noexcept
        : released_(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          creation_error_(released_ ? ERROR_SUCCESS : GetLastError()) {}

    [[nodiscard]] HRESULT creation_result() const noexcept {
        return released_
            ? S_OK
            : HRESULT_FROM_WIN32(
                creation_error_ ? creation_error_ : ERROR_INVALID_HANDLE);
    }

    [[nodiscard]] DWORD wait(DWORD timeout_ms) const noexcept {
        return released_
            ? WaitForSingleObject(released_, timeout_ms) : WAIT_FAILED;
    }

    STDMETHODIMP QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFAsyncCallback)) {
            *value = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override {
        const auto remaining = references_.fetch_sub(
            1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return remaining;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override {
        return E_NOTIMPL;
    }

    STDMETHODIMP Invoke(IMFAsyncResult*) override {
        if (released_) SetEvent(released_);
        return S_OK;
    }

private:
    ~TrackedReleaseCallback() {
        if (released_) CloseHandle(released_);
    }
    std::atomic<ULONG> references_{1};
    HANDLE released_{};
    DWORD creation_error_{};
};

D3D11CastCapabilityCheck check(HRESULT result) noexcept {
    return D3D11CastCapabilityCheck{
        true,
        SUCCEEDED(result),
        static_cast<std::int32_t>(result),
    };
}

std::uint64_t luid_value(LUID luid) noexcept {
    return (static_cast<std::uint64_t>(
        static_cast<std::uint32_t>(luid.HighPart)) << 32U)
        | static_cast<std::uint32_t>(luid.LowPart);
}

HRESULT adapter_description(
        ID3D11Device* device,
        D3D11CastCapabilityReport& report) {
    IDXGIDevice* dxgi_device{};
    IDXGIAdapter* adapter{};
    auto hr = device
        ? device->QueryInterface(IID_PPV_ARGS(&dxgi_device)) : E_POINTER;
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(hr)) hr = adapter->GetDesc(&description);
    if (SUCCEEDED(hr)) {
        report.adapter_luid = luid_value(description.AdapterLuid);
        report.vendor_id = description.VendorId;
        report.device_id = description.DeviceId;
        report.feature_level = static_cast<std::uint32_t>(
            device->GetFeatureLevel());
    }
    release(adapter);
    release(dxgi_device);
    return hr;
}

std::uint32_t windows_build_number() noexcept {
    using RtlGetVersionFunction = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto function = module ? reinterpret_cast<RtlGetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion")) : nullptr;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    return function && function(&version) == 0 ? version.dwBuildNumber : 0;
}

HRESULT create_nv12_texture(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        UINT bind_flags,
        ID3D11Texture2D** texture) {
    if (!device || !texture) return E_POINTER;
    *texture = nullptr;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_NV12;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bind_flags;
    return device->CreateTexture2D(&description, nullptr, texture);
}

HRESULT probe_plane_srvs(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height) {
    const auto probe_bindings = [=](UINT bind_flags) {
        ID3D11Texture2D* texture{};
        auto hr = create_nv12_texture(
            device, width, height,
            bind_flags | D3D11_BIND_SHADER_RESOURCE, &texture);
        ID3D11ShaderResourceView* y{};
        ID3D11ShaderResourceView* uv{};
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        view.Format = DXGI_FORMAT_R8_UNORM;
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(texture, &view, &y);
        }
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(hr)) {
            hr = device->CreateShaderResourceView(texture, &view, &uv);
        }
        release(uv);
        release(y);
        release(texture);
        return hr;
    };
    auto hr = probe_bindings(D3D11_BIND_DECODER);
    if (SUCCEEDED(hr)) {
        hr = probe_bindings(D3D11_BIND_RENDER_TARGET);
    }
    return hr;
}

HRESULT probe_plane_uavs(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        UINT extra_bind_flags,
        ID3D11Texture2D** retained_texture = nullptr) {
    if (retained_texture) *retained_texture = nullptr;
    ID3D11Texture2D* texture{};
    auto hr = create_nv12_texture(device, width, height,
        D3D11_BIND_UNORDERED_ACCESS | extra_bind_flags, &texture);
    ID3D11UnorderedAccessView* y{};
    ID3D11UnorderedAccessView* uv{};
    D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
    view.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    view.Format = DXGI_FORMAT_R8_UNORM;
    if (SUCCEEDED(hr)) {
        hr = device->CreateUnorderedAccessView(texture, &view, &y);
    }
    view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (SUCCEEDED(hr)) {
        hr = device->CreateUnorderedAccessView(texture, &view, &uv);
    }
    release(uv);
    release(y);
    if (SUCCEEDED(hr) && retained_texture) {
        *retained_texture = texture;
        texture = nullptr;
    }
    release(texture);
    return hr;
}

HRESULT probe_encoder_copy(
        ID3D11Device* device,
        std::uint32_t width,
        std::uint32_t height,
        ID3D11Texture2D** retained_encoder_texture = nullptr) {
    if (retained_encoder_texture) *retained_encoder_texture = nullptr;
    ID3D11Texture2D* compose{};
    ID3D11Texture2D* encoder{};
    auto hr = create_nv12_texture(
        device, width, height, D3D11_BIND_UNORDERED_ACCESS, &compose);
    if (SUCCEEDED(hr)) {
        hr = create_nv12_texture(
            device, width, height, D3D11_BIND_VIDEO_ENCODER, &encoder);
    }
    ID3D11DeviceContext* context{};
    if (SUCCEEDED(hr)) device->GetImmediateContext(&context);
    if (SUCCEEDED(hr) && !context) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        context->CopyResource(encoder, compose);
        hr = device->GetDeviceRemovedReason();
    }
    release(context);
    release(compose);
    if (SUCCEEDED(hr) && retained_encoder_texture) {
        *retained_encoder_texture = encoder;
        encoder = nullptr;
    }
    release(encoder);
    return hr;
}

HRESULT probe_nv12_video_processor(
        ID3D11Device* device,
        const D3D11CastCapabilityProbeOptions& options) {
    ID3D11VideoDevice* video_device{};
    auto hr = device->QueryInterface(IID_PPV_ARGS(&video_device));
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate.Numerator = options.frame_rate_num;
    content.InputFrameRate.Denominator = options.frame_rate_den;
    content.InputWidth = options.width;
    content.InputHeight = options.height;
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = options.width;
    content.OutputHeight = options.height;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    ID3D11VideoProcessorEnumerator* enumerator{};
    if (SUCCEEDED(hr)) {
        hr = video_device->CreateVideoProcessorEnumerator(
            &content, &enumerator);
    }
    UINT support{};
    if (SUCCEEDED(hr)) {
        hr = enumerator->CheckVideoProcessorFormat(
            DXGI_FORMAT_NV12, &support);
    }
    constexpr UINT required = D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT
        | D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT;
    if (SUCCEEDED(hr) && (support & required) != required) hr = E_FAIL;
    UINT bgra_support{};
    if (SUCCEEDED(hr)) {
        hr = enumerator->CheckVideoProcessorFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, &bgra_support);
    }
    if (SUCCEEDED(hr)
            && (bgra_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)
                == 0) {
        hr = E_FAIL;
    }
    ID3D11VideoProcessor* processor{};
    if (SUCCEEDED(hr)) {
        hr = video_device->CreateVideoProcessor(enumerator, 0, &processor);
    }
    release(processor);
    release(enumerator);
    release(video_device);
    return hr;
}

std::wstring source_url(const wchar_t* source) {
    if (!source || !*source) return {};
    std::wstring value(source);
    if (value.starts_with(L"http://") || value.starts_with(L"https://")) {
        return value;
    }
    DWORD length = 4096;
    std::wstring url(length, L'\0');
    if (FAILED(UrlCreateFromPathW(
            value.c_str(), url.data(), &length, 0))) {
        return value;
    }
    url.resize(length);
    return url;
}

HRESULT same_adapter(ID3D11Device* left, ID3D11Device* right) {
    if (!left || !right) return E_POINTER;
    D3D11CastCapabilityReport left_report;
    D3D11CastCapabilityReport right_report;
    auto hr = adapter_description(left, left_report);
    if (SUCCEEDED(hr)) hr = adapter_description(right, right_report);
    if (SUCCEEDED(hr) && left_report.adapter_luid != right_report.adapter_luid) {
        hr = DXGI_ERROR_INVALID_CALL;
    }
    return hr;
}

HRESULT probe_native_nv12_source(
        ID3D11Device* device,
        const wchar_t* source) {
    if (!source || !*source) return E_INVALIDARG;
    IMFDXGIDeviceManager* manager{};
    UINT reset_token{};
    auto hr = MFCreateDXGIDeviceManager(&reset_token, &manager);
    if (SUCCEEDED(hr)) hr = manager->ResetDevice(device, reset_token);
    IMFAttributes* attributes{};
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager);
    }
    if (SUCCEEDED(hr)) {
        hr = attributes->SetUINT32(
            kReadwriteEnableHardwareTransforms, TRUE);
    }
    IMFSourceReader* reader{};
    const auto url = source_url(source);
    if (SUCCEEDED(hr)) {
        hr = MFCreateSourceReaderFromURL(url.c_str(), attributes, &reader);
    }
    if (SUCCEEDED(hr)) {
        static_cast<void>(reader->SetStreamSelection(
            MF_SOURCE_READER_ALL_STREAMS, FALSE));
        hr = reader->SetStreamSelection(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    }
    IMFMediaType* native{};
    if (SUCCEEDED(hr)) {
        static_cast<void>(reader->GetNativeMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native));
        HRESULT media_type_result = E_FAIL;
        for (const bool pin_source_geometry : {true, false}) {
            IMFMediaType* type{};
            auto type_result = MFCreateMediaType(&type);
            if (SUCCEEDED(type_result)) {
                type_result = type->SetGUID(
                    MF_MT_MAJOR_TYPE, MFMediaType_Video);
            }
            if (SUCCEEDED(type_result)) {
                type_result = type->SetGUID(
                    MF_MT_SUBTYPE, MFVideoFormat_NV12);
            }
            if (SUCCEEDED(type_result)) {
                static_cast<void>(type->SetUINT32(
                    MF_SA_D3D11_BINDFLAGS,
                    D3D11_BIND_SHADER_RESOURCE));
            }
            if (SUCCEEDED(type_result) && pin_source_geometry && native) {
                UINT32 width{}, height{};
                if (SUCCEEDED(MFGetAttributeSize(
                            native, MF_MT_FRAME_SIZE, &width, &height))
                        && width != 0 && height != 0) {
                    static_cast<void>(MFSetAttributeSize(
                        type, MF_MT_FRAME_SIZE, width, height));
                }
                UINT32 numerator{}, denominator{};
                if (SUCCEEDED(MFGetAttributeRatio(native,
                            MF_MT_PIXEL_ASPECT_RATIO,
                            &numerator, &denominator))
                        && numerator != 0 && denominator != 0) {
                    static_cast<void>(MFSetAttributeRatio(type,
                        MF_MT_PIXEL_ASPECT_RATIO, numerator, denominator));
                }
                if (SUCCEEDED(MFGetAttributeRatio(native,
                            MF_MT_FRAME_RATE,
                            &numerator, &denominator))
                        && numerator != 0 && denominator != 0) {
                    static_cast<void>(MFSetAttributeRatio(type,
                        MF_MT_FRAME_RATE, numerator, denominator));
                }
                UINT32 interlace{};
                if (SUCCEEDED(native->GetUINT32(
                            MF_MT_INTERLACE_MODE, &interlace))) {
                    static_cast<void>(type->SetUINT32(
                        MF_MT_INTERLACE_MODE, interlace));
                }
            }
            if (SUCCEEDED(type_result)) {
                media_type_result = reader->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
            } else {
                media_type_result = type_result;
            }
            release(type);
            if (SUCCEEDED(media_type_result)) break;
        }
        hr = media_type_result;
    }

    IMFSample* sample{};
    for (unsigned attempt = 0; SUCCEEDED(hr) && !sample && attempt < 128;
            ++attempt) {
        DWORD actual_stream{};
        DWORD flags{};
        LONGLONG timestamp{};
        hr = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
            &actual_stream, &flags, &timestamp, &sample);
        static_cast<void>(actual_stream);
        static_cast<void>(timestamp);
        if (SUCCEEDED(hr) && (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            hr = kMfEndOfStream;
        }
    }
    IMFMediaBuffer* buffer{};
    IMFDXGIBuffer* dxgi_buffer{};
    ID3D11Texture2D* texture{};
    if (SUCCEEDED(hr) && sample) hr = sample->GetBufferByIndex(0, &buffer);
    if (SUCCEEDED(hr)) hr = buffer->QueryInterface(IID_PPV_ARGS(&dxgi_buffer));
    if (SUCCEEDED(hr)) hr = dxgi_buffer->GetResource(IID_PPV_ARGS(&texture));
    D3D11_TEXTURE2D_DESC description{};
    if (SUCCEEDED(hr)) texture->GetDesc(&description);
    if (SUCCEEDED(hr) && description.Format != DXGI_FORMAT_NV12) hr = E_FAIL;
    ID3D11Device* texture_device{};
    if (SUCCEEDED(hr)) texture->GetDevice(&texture_device);
    if (SUCCEEDED(hr)) hr = same_adapter(device, texture_device);

    release(texture_device);
    release(texture);
    release(dxgi_buffer);
    release(buffer);
    release(sample);
    release(native);
    release(reader);
    release(attributes);
    release(manager);
    return hr;
}

IMFMediaType* video_type(
        REFGUID subtype,
        const D3D11CastCapabilityProbeOptions& options,
        bool encoded) {
    IMFMediaType* type{};
    if (FAILED(MFCreateMediaType(&type))) return nullptr;
    auto hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeSize(
            type, MF_MT_FRAME_SIZE, options.width, options.height);
    }
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE,
            options.frame_rate_num, options.frame_rate_den);
    }
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(
        type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(
        MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr) && encoded) {
        hr = type->SetUINT32(MF_MT_AVG_BITRATE, 6000000);
    }
    if (FAILED(hr)) release(type);
    return type;
}

HRESULT wait_for_need_input(
        IMFMediaEventGenerator* events,
        std::chrono::milliseconds timeout) {
    if (!events) return E_POINTER;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        IMFMediaEvent* event{};
        const auto result = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (result == kMfNoEventsAvailable) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (FAILED(result)) return result;
        MediaEventType type{};
        HRESULT status{};
        auto hr = event->GetType(&type);
        if (SUCCEEDED(hr)) hr = event->GetStatus(&status);
        release(event);
        if (FAILED(hr)) return hr;
        if (FAILED(status)) return status;
        if (type == METransformNeedInput) return S_OK;
    }
    return MF_E_NOTACCEPTING;
}

HRESULT probe_h264_surface(
        ID3D11Device* device,
        ID3D11Texture2D* texture,
        const D3D11CastCapabilityProbeOptions& options,
        D3D11CastCapabilityCheck* tracked_release,
        bool* retained_asynchronously) {
    if (!device || !texture) return E_POINTER;
    if (tracked_release) *tracked_release = {};
    if (retained_asynchronously) *retained_asynchronously = false;
    IMFDXGIDeviceManager* manager{};
    UINT reset_token{};
    auto hr = MFCreateDXGIDeviceManager(&reset_token, &manager);
    if (SUCCEEDED(hr)) hr = manager->ResetDevice(device, reset_token);

    MFT_REGISTER_TYPE_INFO input_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output_info{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations{};
    UINT activation_count{};
    if (SUCCEEDED(hr)) {
        hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
            MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
            &input_info, &output_info, &activations, &activation_count);
    }
    HRESULT last = FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT index = 0; index < activation_count; ++index) {
        IMFTransform* encoder{};
        IMFMediaEventGenerator* events{};
        IMFAttributes* attributes{};
        hr = activations[index]->ActivateObject(IID_PPV_ARGS(&encoder));
        if (SUCCEEDED(hr)) hr = encoder->GetAttributes(&attributes);
        UINT32 asynchronous{};
        UINT32 d3d11_aware{};
        if (SUCCEEDED(hr)) {
            static_cast<void>(attributes->GetUINT32(
                MF_TRANSFORM_ASYNC, &asynchronous));
            static_cast<void>(attributes->GetUINT32(
                MF_SA_D3D11_AWARE, &d3d11_aware));
            if (!asynchronous || !d3d11_aware) hr = E_NOINTERFACE;
        }
        if (SUCCEEDED(hr)) hr = attributes->SetUINT32(
            MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (SUCCEEDED(hr)) hr = encoder->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(manager));
        auto* output_type = video_type(MFVideoFormat_H264, options, true);
        auto* input_type = video_type(MFVideoFormat_NV12, options, false);
        if (SUCCEEDED(hr)) hr = output_type
            ? encoder->SetOutputType(0, output_type, 0) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) hr = input_type
            ? encoder->SetInputType(0, input_type, 0) : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            hr = encoder->QueryInterface(IID_PPV_ARGS(&events));
        }
        if (SUCCEEDED(hr)) hr = encoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (SUCCEEDED(hr)) hr = encoder->ProcessMessage(
            MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (SUCCEEDED(hr)) {
            hr = wait_for_need_input(events, std::chrono::milliseconds(500));
        }
        IMFMediaBuffer* buffer{};
        IMFSample* sample{};
        IMFTrackedSample* tracked{};
        TrackedReleaseCallback* release_callback{};
        if (SUCCEEDED(hr)) {
            hr = MFCreateDXGISurfaceBuffer(
                __uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
        }
        if (SUCCEEDED(hr)) {
            release_callback = new (std::nothrow) TrackedReleaseCallback();
            if (!release_callback) {
                hr = E_OUTOFMEMORY;
            } else {
                hr = release_callback->creation_result();
            }
        }
        if (SUCCEEDED(hr)) hr = MFCreateTrackedSample(&tracked);
        if (SUCCEEDED(hr)) {
            hr = tracked->QueryInterface(IID_PPV_ARGS(&sample));
        }
        if (SUCCEEDED(hr)) {
            hr = tracked->SetAllocator(release_callback, nullptr);
        }
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
        if (SUCCEEDED(hr)) hr = sample->SetSampleTime(0);
        if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(
            static_cast<LONGLONG>(10000000ULL
                * options.frame_rate_den / options.frame_rate_num));
        if (SUCCEEDED(hr)) hr = encoder->ProcessInput(0, sample, 0);
        last = hr;

        release(sample);
        release(tracked);
        release(buffer);
        const bool async_retention = SUCCEEDED(last) && release_callback
            && release_callback->wait(0) == WAIT_TIMEOUT;
        if (encoder) {
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_COMMAND_FLUSH, 0));
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
            static_cast<void>(encoder->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER, 0));
        }
        release(input_type);
        release(output_type);
        release(attributes);
        release(events);
        release(encoder);
        HRESULT release_result = FAILED(last) ? last : E_FAIL;
        if (SUCCEEDED(last) && release_callback) {
            release_result = release_callback->wait(500)
                    == WAIT_OBJECT_0
                ? S_OK : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (tracked_release) *tracked_release = check(release_result);
        if (retained_asynchronously) {
            *retained_asynchronously = async_retention;
        }
        release(release_callback);
        if (SUCCEEDED(last) && SUCCEEDED(release_result)) break;
        if (SUCCEEDED(last)) last = release_result;
    }
    for (UINT index = 0; index < activation_count; ++index) {
        release(activations[index]);
    }
    CoTaskMemFree(activations);
    release(manager);
    return last;
}

}  // namespace

D3D11CastCapabilityReport D3D11CastCapabilityProbe::run(
        void* d3d11_device,
        const D3D11CastCapabilityProbeOptions& options) {
    D3D11CastCapabilityReport report;
    auto* device = static_cast<ID3D11Device*>(d3d11_device);
    if (!device || options.width == 0 || options.height == 0
            || options.frame_rate_num == 0 || options.frame_rate_den == 0) {
        report.device_healthy = check(E_INVALIDARG);
        return select_d3d11_cast_backend(report);
    }

    report.windows_build = windows_build_number();
    auto hr = adapter_description(device, report);
    if (SUCCEEDED(hr)) hr = device->GetDeviceRemovedReason();
    report.device_healthy = check(hr);
    report.even_420_geometry = check(
        (options.width & 1U) == 0 && (options.height & 1U) == 0
            ? S_OK : E_INVALIDARG);
    report.nv12_plane_srvs = check(
        probe_plane_srvs(device, options.width, options.height));
    report.nv12_plane_uavs = check(
        probe_plane_uavs(device, options.width, options.height, 0));

    ID3D11Texture2D* direct_encoder_texture{};
    hr = probe_plane_uavs(device, options.width, options.height,
        D3D11_BIND_VIDEO_ENCODER, &direct_encoder_texture);
    report.uav_video_encoder_bind = check(hr);
    report.nv12_video_processor = check(
        probe_nv12_video_processor(device, options));

    if (options.decoder_source && *options.decoder_source) {
        report.native_nv12_source = check(
            probe_native_nv12_source(device, options.decoder_source));
    }
    if (options.probe_hardware_encoder) {
        D3D11CastCapabilityCheck tracked_release;
        bool retained_asynchronously{};
        hr = probe_h264_surface(
            device, direct_encoder_texture, options,
            &tracked_release, &retained_asynchronously);
        report.combined_uav_encoder_input = check(hr);
        if (SUCCEEDED(hr)) {
            report.dxgi_h264_encoder = check(hr);
            report.tracked_sample_release = tracked_release;
            report.encoder_retains_samples_async = retained_asynchronously;
        } else {
            ID3D11Texture2D* copied_encoder_texture{};
            const auto copy_result = probe_encoder_copy(
                device, options.width, options.height,
                &copied_encoder_texture);
            report.compose_to_encoder_copy = check(copy_result);
            const auto encoder_result = SUCCEEDED(copy_result)
                ? probe_h264_surface(
                    device, copied_encoder_texture, options,
                    &tracked_release, &retained_asynchronously)
                : copy_result;
            report.dxgi_h264_encoder = check(encoder_result);
            report.tracked_sample_release = tracked_release;
            report.encoder_retains_samples_async = retained_asynchronously;
            release(copied_encoder_texture);
        }
    } else if (!report.uav_video_encoder_bind.passed()) {
        ID3D11Texture2D* copied_encoder_texture{};
        report.compose_to_encoder_copy = check(probe_encoder_copy(
            device, options.width, options.height,
            &copied_encoder_texture));
        release(copied_encoder_texture);
    }
    release(direct_encoder_texture);
    return select_d3d11_cast_backend(report);
}

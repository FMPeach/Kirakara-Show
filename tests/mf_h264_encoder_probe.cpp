#include "../apps/show_host/cast/cast_stage_renderer.h"
#include "../apps/show_host/cast/d3d11_bgra_to_nv12.h"
#include "../apps/show_host/cast/mf_d3d11_h264_encoder.h"
#include "../apps/show_host/cast/cast_pipeline_diagnostics.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <objbase.h>

namespace {

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void expect(HRESULT result, const char* message) {
    if (FAILED(result)) {
        std::cerr << message << " 0x" << std::hex
                  << static_cast<unsigned long>(result) << '\n';
        std::exit(1);
    }
}

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return value;
}

ID3D11Device* create_device_for_adapter(
        const std::wstring& adapter_filter,
        std::wstring& selected_name) {
    IDXGIFactory1* factory{};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    ID3D11Device* selected_device{};
    const auto needle = lowercase(adapter_filter);
    for (UINT index = 0; !selected_device; ++index) {
        IDXGIAdapter1* adapter{};
        const auto enum_result = factory->EnumAdapters1(index, &adapter);
        if (enum_result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_result) || !adapter) continue;

        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description))) {
            release(adapter);
            continue;
        }
        const std::wstring name = description.Description;
        std::wcout << L"dxgi_adapter[" << index << L"]=" << name << L'\n';
        const bool hardware =
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
        if (hardware && lowercase(name).find(needle) != std::wstring::npos) {
            constexpr D3D_FEATURE_LEVEL levels[]{
                D3D_FEATURE_LEVEL_11_1,
                D3D_FEATURE_LEVEL_11_0,
            };
            D3D_FEATURE_LEVEL selected_level{};
            const auto create_result = D3D11CreateDevice(
                adapter,
                D3D_DRIVER_TYPE_UNKNOWN,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT
                    | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                levels,
                static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                &selected_device,
                &selected_level,
                nullptr);
            if (SUCCEEDED(create_result)) selected_name = name;
        }
        release(adapter);
    }
    release(factory);
    return selected_device;
}

IMFMediaType* make_video_type(
        const GUID& subtype, bool compressed, std::uint32_t bitrate = 0) {
    IMFMediaType* type{};
    auto hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, 1920, 1080);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, 60, 1);
    if (SUCCEEDED(hr)) {
        hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(
            MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(hr) && compressed) {
        hr = type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    }
    if (FAILED(hr)) {
        release(type);
        return nullptr;
    }
    return type;
}

IMFSample* make_nv12_sample(
        ID3D11Texture2D* texture,
        LONGLONG timestamp,
        LONGLONG duration,
        bool discontinuity) {
    IMFMediaBuffer* buffer{};
    auto hr = MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
    IMFSample* sample{};
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(timestamp);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr) && discontinuity) {
        hr = sample->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
    }
    release(buffer);
    if (FAILED(hr)) release(sample);
    return sample;
}

bool pull_encoded_output(
        IMFTransform* encoder,
        std::vector<std::uint8_t>& encoded,
        bool& keyframe) {
    MFT_OUTPUT_STREAM_INFO info{};
    auto hr = encoder->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return false;

    IMFSample* caller_sample{};
    if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
        hr = MFCreateSample(&caller_sample);
        IMFMediaBuffer* buffer{};
        const auto capacity = std::max<DWORD>(info.cbSize, 2U * 1024U * 1024U);
        if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(capacity, &buffer);
        if (SUCCEEDED(hr)) hr = caller_sample->AddBuffer(buffer);
        release(buffer);
    }
    if (FAILED(hr)) {
        release(caller_sample);
        return false;
    }

    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = caller_sample;
    DWORD status{};
    hr = encoder->ProcessOutput(0, 1, &output, &status);
    static_cast<void>(status);
    if (FAILED(hr)) {
        release(output.pEvents);
        release(output.pSample);
        return false;
    }

    IMFMediaBuffer* contiguous{};
    hr = output.pSample
        ? output.pSample->ConvertToContiguousBuffer(&contiguous) : E_FAIL;
    BYTE* bytes{};
    DWORD byte_count{};
    if (SUCCEEDED(hr)) hr = contiguous->Lock(&bytes, nullptr, &byte_count);
    if (SUCCEEDED(hr) && byte_count > 0) {
        encoded.assign(bytes, bytes + byte_count);
        UINT32 clean{};
        keyframe = SUCCEEDED(output.pSample->GetUINT32(
            MFSampleExtension_CleanPoint, &clean)) && clean != 0;
    }
    if (bytes) contiguous->Unlock();
    release(contiguous);
    release(output.pEvents);
    release(output.pSample);
    return SUCCEEDED(hr) && !encoded.empty();
}

bool encode_probe_frame(IMFTransform* encoder, ID3D11Texture2D* texture,
        std::uint32_t frame_rate) {
    IMFMediaEventGenerator* events{};
    auto hr = encoder->QueryInterface(IID_PPV_ARGS(&events));
    if (FAILED(hr) || !events) return false;
    hr = encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr)) {
        hr = encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (FAILED(hr)) {
        release(events);
        return false;
    }

    const auto frame_duration = 10000000LL
        / static_cast<LONGLONG>(std::max<std::uint32_t>(1, frame_rate));
    int submitted{};
    bool encoded_frame = false;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (!encoded_frame && std::chrono::steady_clock::now() < deadline) {
        IMFMediaEvent* event{};
        hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            DWORD input_status{};
            if (submitted < 8
                    && SUCCEEDED(encoder->GetInputStatus(0, &input_status))
                    && (input_status & MFT_INPUT_STATUS_ACCEPT_DATA) != 0) {
                auto* sample = make_nv12_sample(
                    texture,
                    static_cast<LONGLONG>(submitted) * frame_duration,
                    frame_duration,
                    submitted == 0);
                const auto input_result = sample
                    ? encoder->ProcessInput(0, sample, 0) : E_OUTOFMEMORY;
                release(sample);
                if (FAILED(input_result)) break;
                if (submitted == 0) {
                    std::wcout << L"  proactive_input=accepted\n";
                }
                ++submitted;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (FAILED(hr) || !event) break;

        MediaEventType type{MEUnknown};
        HRESULT event_status{};
        static_cast<void>(event->GetType(&type));
        static_cast<void>(event->GetStatus(&event_status));
        release(event);
        if (FAILED(event_status)) break;

        if (type == METransformNeedInput && submitted < 8) {
            auto* sample = make_nv12_sample(
                texture,
                static_cast<LONGLONG>(submitted) * frame_duration,
                frame_duration,
                submitted == 0);
            if (!sample) break;
            hr = encoder->ProcessInput(0, sample, 0);
            release(sample);
            if (FAILED(hr)) break;
            ++submitted;
        } else if (type == METransformHaveOutput) {
            std::vector<std::uint8_t> encoded;
            bool keyframe{};
            if (!pull_encoded_output(encoder, encoded, keyframe)) break;
            std::wcout << L"  encoded_bytes=" << encoded.size()
                       << L" keyframe=" << keyframe << L" head=";
            const auto preview = std::min<std::size_t>(16, encoded.size());
            for (std::size_t i = 0; i < preview; ++i) {
                std::wcout << std::hex << std::setw(2) << std::setfill(L'0')
                           << static_cast<unsigned>(encoded[i]);
            }
            std::wcout << std::dec << L'\n';
            encoded_frame = true;
        }
    }

    static_cast<void>(encoder->ProcessMessage(
        MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
    static_cast<void>(encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
    static_cast<void>(encoder->ProcessMessage(
        MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
    release(events);
    return encoded_frame;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Keep adapter-specific diagnostics visible even if a vendor MFT blocks.
    std::wcout << std::unitbuf;
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    expect(com, "CoInitializeEx failed");
    const auto mf = MFStartup(MF_VERSION);
    expect(mf, "MFStartup failed");

    MFT_REGISTER_TYPE_INFO input_info{
        MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output_info{
        MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activations{};
    UINT32 activation_count{};
    auto hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &input_info,
        &output_info,
        &activations,
        &activation_count);
    expect(hr, "MFTEnumEx hardware H264 failed");
    std::wcout << L"hardware_h264_encoders=" << activation_count << L'\n';
    if (activation_count == 0) {
        CoTaskMemFree(activations);
        MFShutdown();
        CoUninitialize();
        return 2;
    }

    CastStageRenderer renderer;
    kirakara::show::win32::StageCanvas canvas{};
    std::wstring adapter_filter;
    bool force_readback{};
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--fps=30") == 0) {
            canvas.frame_rate_num = 30;
        } else if (std::wcsncmp(argv[i], L"--adapter=", 10) == 0) {
            adapter_filter = argv[i] + 10;
        } else if (std::wcscmp(argv[i], L"--readback") == 0) {
            force_readback = true;
        }
    }
    std::wcout << L"canvas=" << canvas.width << L'x' << canvas.height
               << L'@' << canvas.frame_rate_num << L'\n';
    ID3D11Device* selected_device{};
    if (!adapter_filter.empty()) {
        std::wstring selected_name;
        selected_device = create_device_for_adapter(
            adapter_filter, selected_name);
        expect(selected_device ? S_OK : E_FAIL,
            "requested D3D11 adapter not found");
        std::wcout << L"requested_adapter=" << selected_name << L'\n';
        expect(renderer.compositor().configure(canvas, selected_device)
                ? S_OK : E_FAIL,
            "CastStageRenderer selected-adapter configure failed");
    } else {
        expect(renderer.configure(canvas) ? S_OK : E_FAIL,
            "CastStageRenderer configure failed");
    }
    auto* device = static_cast<ID3D11Device*>(renderer.native_d3d_device());
    expect(device ? S_OK : E_POINTER, "Cast D3D11 device missing");
    IDXGIDevice* dxgi_device{};
    IDXGIAdapter* adapter{};
    DXGI_ADAPTER_DESC adapter_desc{};
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))
            && SUCCEEDED(dxgi_device->GetAdapter(&adapter))
            && SUCCEEDED(adapter->GetDesc(&adapter_desc))) {
        std::wcout << L"d3d11_adapter=" << adapter_desc.Description
                   << L" luid=" << std::hex
                   << static_cast<unsigned long>(adapter_desc.AdapterLuid.HighPart)
                   << L':' << adapter_desc.AdapterLuid.LowPart << std::dec
                   << L'\n';
    }
    release(adapter);
    release(dxgi_device);
    expect(renderer.compose_idle_frame() ? S_OK : E_FAIL,
        "Cast idle frame compose failed");
    renderer.finalize_frame();

    D3D11BgraToNv12Converter converter;
    expect(converter.configure(
            device,
            canvas.width,
            canvas.height,
            canvas.frame_rate_num,
            canvas.frame_rate_den)
            ? S_OK : E_FAIL,
        "D3D11 BGRA to NV12 configure failed");
    std::wcout << L"vp_submission_flush="
               << (converter.flush_mode()
                        == D3D11CastFlushMode::video_context
                    ? L"video-context"
                    : L"full-context")
               << L'\n';
    void* nv12_texture{};
    expect(converter.create_output_texture(&nv12_texture) ? S_OK : E_FAIL,
        "NV12 texture allocation failed");
    expect(converter.convert(
            renderer.compositor().current_frame().overlay_texture,
            nv12_texture) ? S_OK : E_FAIL,
        "GPU BGRA to NV12 conversion failed");
    std::wcout << L"gpu_bgra_to_nv12=ok\n";
    auto* owned_nv12 = static_cast<ID3D11Texture2D*>(nv12_texture);

    IMFDXGIDeviceManager* device_manager{};
    UINT reset_token{};
    hr = MFCreateDXGIDeviceManager(&reset_token, &device_manager);
    if (SUCCEEDED(hr)) hr = device_manager->ResetDevice(device, reset_token);
    expect(hr, "MFCreateDXGIDeviceManager failed");

    bool compatible_found = false;
    for (UINT32 i = 0; i < activation_count; ++i) {
        wchar_t* name{};
        UINT32 name_length{};
        static_cast<void>(activations[i]->GetAllocatedString(
            MFT_FRIENDLY_NAME_Attribute, &name, &name_length));
        std::wcout << L"encoder[" << i << L"]="
                   << (name ? name : L"(unnamed)") << L'\n';
        CoTaskMemFree(name);

        IMFTransform* encoder{};
        hr = activations[i]->ActivateObject(IID_PPV_ARGS(&encoder));
        if (FAILED(hr) || !encoder) {
            std::wcout << L"  activate=0x" << std::hex
                       << static_cast<unsigned long>(hr) << std::dec << L'\n';
            continue;
        }

        IMFAttributes* attributes{};
        UINT32 asynchronous{};
        UINT32 d3d11_aware{};
        if (SUCCEEDED(encoder->GetAttributes(&attributes))) {
            static_cast<void>(attributes->GetUINT32(
                MF_TRANSFORM_ASYNC, &asynchronous));
            static_cast<void>(attributes->GetUINT32(
                MF_SA_D3D11_AWARE, &d3d11_aware));
            if (asynchronous) {
                static_cast<void>(attributes->SetUINT32(
                    MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
            }
        }
        std::wcout << L"  async=" << asynchronous
                   << L" d3d11_aware=" << d3d11_aware << L'\n';

        HRESULT d3d_result = S_FALSE;
        if (d3d11_aware) {
            d3d_result = encoder->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(device_manager));
        }

        auto* output_type = make_video_type(
            MFVideoFormat_H264, true, 6000000);
        auto* input_type = make_video_type(MFVideoFormat_NV12, false);
        HRESULT output_result = output_type
            ? encoder->SetOutputType(0, output_type, 0) : E_OUTOFMEMORY;
        HRESULT input_result = SUCCEEDED(output_result) && input_type
            ? encoder->SetInputType(0, input_type, 0) : E_FAIL;
        std::wcout << L"  set_d3d=0x" << std::hex
                   << static_cast<unsigned long>(d3d_result)
                   << L" output_type=0x"
                   << static_cast<unsigned long>(output_result)
                   << L" input_type=0x"
                   << static_cast<unsigned long>(input_result)
                   << std::dec << L'\n';
        compatible_found = compatible_found
            || (d3d11_aware && SUCCEEDED(d3d_result)
                && SUCCEEDED(output_result) && SUCCEEDED(input_result));
        if (d3d11_aware && SUCCEEDED(d3d_result)
                && SUCCEEDED(output_result) && SUCCEEDED(input_result)) {
            const bool encoded = encode_probe_frame(
                encoder, owned_nv12, canvas.frame_rate_num);
            std::wcout << L"  encode_probe=" << (encoded ? L"ok" : L"failed")
                       << L'\n';
            compatible_found = compatible_found && encoded;
        }

        release(input_type);
        release(output_type);
        release(attributes);
        release(encoder);
    }

    std::mutex production_mutex;
    std::condition_variable production_ready;
    bool production_encoded{};
    bool production_annex_b{};
    std::size_t production_bytes{};
    std::int64_t production_forced_min_pts90k{
        std::numeric_limits<std::int64_t>::max()};
    bool production_forced_keyframe{};
    MfH264ForceKeyframeSupport production_keyframe_support{
        MfH264ForceKeyframeSupport::unknown};
    MfD3D11H264Encoder production_encoder;
    CastPipelineDiagnostics production_diagnostics;
    MfD3D11H264EncoderConfig production_config{};
    production_config.width = canvas.width;
    production_config.height = canvas.height;
    production_config.frame_rate_num = canvas.frame_rate_num;
    production_config.frame_rate_den = canvas.frame_rate_den;
    production_config.bitrate = 6000000;
    if (force_readback) {
        production_config.preference =
            MfH264EncoderPreference::hardware_readback_only;
    }
    const auto production_started = production_encoder.start(
        device,
        production_config,
        [&](const std::uint8_t* bytes,
                std::size_t size,
                std::int64_t pts90k,
                bool keyframe) {
            const auto annex_b = size >= 4
                && bytes[0] == 0 && bytes[1] == 0
                && ((bytes[2] == 1)
                    || (bytes[2] == 0 && bytes[3] == 1));
            {
                std::lock_guard lock(production_mutex);
                production_encoded = true;
                production_annex_b = annex_b;
                production_bytes = size;
                production_forced_keyframe = production_forced_keyframe
                    || (keyframe
                        && pts90k >= production_forced_min_pts90k);
            }
            production_ready.notify_one();
        },
        &production_diagnostics);
    std::wcout << L"production_encoder_start="
               << (production_started ? L"ok" : L"failed")
               << L" backend="
               << (production_encoder.backend()
                        == MfH264EncoderBackend::hardware
                    ? L"hardware"
                    : production_encoder.backend()
                        == MfH264EncoderBackend::software
                    ? L"software" : L"none")
               << L" input="
               << (force_readback ? L"readback" : L"dxgi")
               << L'\n';

    if (production_started) {
        production_keyframe_support =
            production_encoder.force_keyframe_support();
        std::wcout << L"production_force_keyframe="
                   << (production_keyframe_support
                            == MfH264ForceKeyframeSupport::supported
                        ? L"supported"
                        : production_keyframe_support
                            == MfH264ForceKeyframeSupport::unsupported
                        ? L"unsupported"
                        : production_keyframe_support
                            == MfH264ForceKeyframeSupport::failed
                        ? L"failed" : L"unknown")
                   << L'\n';
        const auto input_prepared = production_encoder.prepare_input_texture(
            renderer.compositor().current_frame().overlay_texture);
        std::wcout << L"production_input_view_prepared="
                   << (input_prepared ? L"yes" : L"no") << L'\n';
        const auto duration = static_cast<std::int64_t>(
            10000000ULL * canvas.frame_rate_den / canvas.frame_rate_num);
        const auto submit_deadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(5);
        for (std::int64_t frame = 0; frame < 16; ++frame) {
            while (!production_encoder.submit_texture(
                    renderer.compositor().current_frame().overlay_texture,
                    frame * duration,
                    duration)) {
                {
                    std::lock_guard lock(production_mutex);
                    if (production_encoded) break;
                }
                if (std::chrono::steady_clock::now() >= submit_deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (std::chrono::steady_clock::now() >= submit_deadline) break;
            std::lock_guard lock(production_mutex);
            if (production_encoded) break;
        }
        {
            std::unique_lock lock(production_mutex);
            static_cast<void>(production_ready.wait_for(
                lock,
                std::chrono::seconds(5),
                [&] { return production_encoded; }));
            std::wcout << L"production_encoder_output="
                       << (production_encoded ? L"ok" : L"failed")
                       << L" bytes=" << production_bytes
                       << L" annex_b=" << production_annex_b << L'\n';
        }

        if (production_keyframe_support
                == MfH264ForceKeyframeSupport::supported) {
            constexpr std::int64_t forced_frame = 32;
            {
                std::lock_guard lock(production_mutex);
                production_forced_min_pts90k =
                    forced_frame * duration * 9LL / 1000LL;
                production_forced_keyframe = false;
            }
            const auto keyframe_deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(5);
            std::int64_t frame = forced_frame;
            while (!production_encoder.submit_texture(
                    renderer.compositor().current_frame().overlay_texture,
                    frame * duration,
                    duration,
                    true)) {
                if (std::chrono::steady_clock::now()
                        >= keyframe_deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            ++frame;
            while (std::chrono::steady_clock::now() < keyframe_deadline) {
                {
                    std::unique_lock lock(production_mutex);
                    if (production_ready.wait_for(
                            lock,
                            std::chrono::milliseconds(5),
                            [&] { return production_forced_keyframe; })) {
                        break;
                    }
                }
                if (production_encoder.submit_texture(
                        renderer.compositor().current_frame().overlay_texture,
                        frame * duration,
                        duration)) {
                    ++frame;
                }
            }
            {
                std::lock_guard lock(production_mutex);
                std::wcout << L"production_forced_keyframe_output="
                           << (production_forced_keyframe
                                ? L"ok" : L"failed")
                           << L'\n';
            }
        }
    }
    const auto production_pipeline = production_diagnostics.snapshot();
    std::wcout << L"production_pool_textures="
               << production_pipeline.counter(
                    CastPipelineCounter::texture_creations)
               << L" production_views="
               << production_pipeline.counter(
                    CastPipelineCounter::view_creations)
               << L" production_encoder_drops="
               << production_pipeline.counter(
                    CastPipelineCounter::encoder_drops)
               << L'\n';
    production_encoder.stop();
    const bool production_compatible = production_started
        && production_encoded && production_annex_b
        && (production_keyframe_support
                != MfH264ForceKeyframeSupport::supported
            || production_forced_keyframe);
    std::wcout << L"synchronous_mft_surface_probe="
               << (compatible_found ? L"ok" : L"unavailable") << L'\n';

    for (UINT32 i = 0; i < activation_count; ++i) {
        release(activations[i]);
    }
    CoTaskMemFree(activations);
    release(owned_nv12);
    release(device_manager);
    release(selected_device);
    MFShutdown();
    CoUninitialize();
    return production_compatible ? 0 : 3;
}

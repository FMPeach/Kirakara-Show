#pragma once
#include "../apps/show_host/overlay/host_hud_state.h"
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <memory>

// A test grid, not a QR encoder. Production images always come from App.
inline std::shared_ptr<HostHudQrImage> make_hud_test_image() {
    using Microsoft::WRL::ComPtr;
    auto image = std::make_shared<HostHudQrImage>();
    image->revision = 1; image->pixel_size = 132; image->modules = 33;
    std::vector<BYTE> pixels(132 * 132 * 4, 255);
    for (int y = 16; y < 116; ++y) for (int x = 16; x < 116; ++x) {
        if ((x / 4 + y / 4) % 2) for (int c = 0; c < 3; ++c) pixels[(y * 132 + x) * 4 + c] = 0;
    }
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
            || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))
            || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
            || FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))
            || FAILED(frame->SetSize(132, 132)) || FAILED(frame->SetPixelFormat(&format))
            || FAILED(frame->WritePixels(132, 132 * 4, static_cast<UINT>(pixels.size()), pixels.data()))
            || FAILED(frame->Commit()) || FAILED(encoder->Commit())) return {};
    STATSTG info{}; HGLOBAL memory{};
    if (FAILED(stream->Stat(&info, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.Get(), &memory))) return {};
    auto* data = static_cast<BYTE*>(GlobalLock(memory));
    if (!data) return {};
    image->png.assign(data, data + info.cbSize.QuadPart);
    GlobalUnlock(memory);
    return image;
}

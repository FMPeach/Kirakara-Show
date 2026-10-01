#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

template <typename T>
void release(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr << "Expected one PNG path.\n";
        return 2;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;

    IWICImagingFactory* factory{};
    IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{};
    IWICFormatConverter* converter{};
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromFilename(argv[1],
        nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) result = converter->Initialize(frame,
        GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeMedianCut);

    UINT width{}, height{};
    if (SUCCEEDED(result)) result = converter->GetSize(&width, &height);
    std::vector<std::uint8_t> pixels;
    if (SUCCEEDED(result) && width == 1280 && height == 720) {
        pixels.resize(static_cast<std::size_t>(width) * height * 4U);
        result = converter->CopyPixels(nullptr, width * 4U,
            static_cast<UINT>(pixels.size()), pixels.data());
    } else if (SUCCEEDED(result)) {
        result = E_FAIL;
    }

    std::size_t background{}, bright{}, dark{}, colored{}, foreground{};
    std::size_t upper_band{}, lower_band{};
    if (SUCCEEDED(result)) {
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4U;
                const auto r = pixels[offset];
                const auto g = pixels[offset + 1];
                const auto b = pixels[offset + 2];
                const bool is_background = r >= 7 && r <= 11
                    && g >= 83 && g <= 87 && b <= 2;
                if (is_background) {
                    ++background;
                    continue;
                }
                ++foreground;
                if (r > 190 && g > 190 && b > 190) ++bright;
                if (r < 35 && g < 35 && b < 35) ++dark;
                if ((g > r + 35 && b > r + 20)
                        || (r > b + 35 && b > g + 20)
                        || (b > r + 35 && b > g + 35)) ++colored;
                if (y >= 350 && y < 535) ++upper_band;
                if (y >= 535 && y < 690) ++lower_band;
            }
        }
    }

    release(converter);
    release(frame);
    release(decoder);
    release(factory);
    CoUninitialize();

    const auto total = static_cast<std::size_t>(1280) * 720;
    const bool valid = SUCCEEDED(result)
        && background > total * 3 / 4
        && foreground > 1000 && bright > 1000 && colored > 100
        && upper_band > 1000 && lower_band > 1000;
    if (!valid) {
        std::cerr << "Invalid rendered frame: bg=" << background
            << " foreground=" << foreground
            << " bright=" << bright << " dark=" << dark
            << " colored=" << colored << " upper=" << upper_band
            << " lower=" << lower_band << "\n";
        return 1;
    }
    std::cout << "Rendered DOM-parity frame signature passed.\n";
    return 0;
}

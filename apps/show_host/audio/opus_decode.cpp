#include "opus_decode.h"
#include "decoded_buffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <string_view>

#include <windows.h>
#include <winhttp.h>
#include <opusfile.h>

namespace {
struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
};

bool starts_with(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(),
        value.begin(), [](wchar_t a, wchar_t b) { return a == std::towlower(b); });
}

bool opus_url(std::wstring_view path) {
    path = path.substr(0, path.find_first_of(L"?#"));
    const auto dot = path.find_last_of(L'.');
    if (dot == path.npos) return false;
    auto extension = std::wstring(path.substr(dot));
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".opus" || extension == L".ogg" || extension == L".oga";
}

struct Input {
    std::unique_ptr<FILE, decltype(&std::fclose)> file{nullptr, &std::fclose};
    InternetHandle session, connection, request;

    bool open_http(const std::wstring& path) {
        URL_COMPONENTS url{};
        url.dwStructSize = sizeof(url);
        url.dwHostNameLength = url.dwUrlPathLength = url.dwExtraInfoLength = -1;
        if (!WinHttpCrackUrl(path.c_str(), 0, 0, &url)
                || (url.nScheme != INTERNET_SCHEME_HTTP && url.nScheme != INTERNET_SCHEME_HTTPS)) return false;
        const std::wstring host(url.lpszHostName, url.dwHostNameLength);
        std::wstring resource(url.lpszUrlPath, url.dwUrlPathLength);
        if (resource.empty()) resource = L"/";
        if (url.dwExtraInfoLength) resource.append(url.lpszExtraInfo, url.dwExtraInfoLength);
        // URL fragments are not part of an HTTP request target.
        if (const auto fragment = resource.find(L'#'); fragment != resource.npos)
            resource.resize(fragment);
        session.value = WinHttpOpen(L"Kirakara-Show/Opus", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 10000, 10000)) return false;
        connection.value = WinHttpConnect(session.value, host.c_str(), url.nPort, 0);
        if (!connection.value) return false;
        request.value = WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            url.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!request.value || !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS,
                0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                || !WinHttpReceiveResponse(request.value, nullptr)) return false;
        DWORD status{}, size = sizeof(status);
        return WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)
            && status >= 200 && status < 300;
    }

    static int read(void* opaque, unsigned char* bytes, int count) {
        if (count <= 0) return 0;
        auto& input = *static_cast<Input*>(opaque);
        if (input.file) {
            const auto received = std::fread(bytes, 1, static_cast<size_t>(count), input.file.get());
            return received == 0 && std::ferror(input.file.get()) ? -1 : static_cast<int>(received);
        }
        DWORD received{};
        return WinHttpReadData(input.request.value, bytes, static_cast<DWORD>(count), &received)
            ? static_cast<int>(received) : -1;
    }
    static int seek(void* opaque, opus_int64 offset, int whence) {
        return _fseeki64(static_cast<Input*>(opaque)->file.get(), offset, whence);
    }
    static opus_int64 tell(void* opaque) {
        return _ftelli64(static_cast<Input*>(opaque)->file.get());
    }
};
}

OpusDecodeResult decode_ogg_opus(const std::wstring& path,
        DecodedAudioBuffer& output, bool allow_unknown_http) {
    Input input;
    const bool http = starts_with(path, L"http://") || starts_with(path, L"https://");
    if (http) {
        if (!allow_unknown_http && !opus_url(path)) return OpusDecodeResult::not_opus;
        if (!input.open_http(path)) return OpusDecodeResult::failed;
    } else {
        input.file.reset(_wfopen(path.c_str(), L"rb"));
        if (!input.file) return OpusDecodeResult::not_opus;
    }
    std::array<unsigned char, 4> prefix{};
    int size = 0;
    while (size < static_cast<int>(prefix.size())) {
        const int count = Input::read(&input, prefix.data() + size,
            static_cast<int>(prefix.size()) - size);
        if (count < 0) return OpusDecodeResult::failed;
        if (count == 0) return OpusDecodeResult::not_opus;
        size += count;
    }
    if (prefix != std::array<unsigned char, 4>{'O', 'g', 'g', 'S'}) return OpusDecodeResult::not_opus;
    const OpusFileCallbacks callbacks{Input::read, http ? nullptr : Input::seek,
        http ? nullptr : Input::tell, nullptr};
    int error{};
    std::unique_ptr<OggOpusFile, decltype(&op_free)> stream(
        op_open_callbacks(&input, &callbacks, prefix.data(), prefix.size(), &error), &op_free);
    if (!stream) return error == OP_ENOTFORMAT
        ? OpusDecodeResult::not_opus : OpusDecodeResult::failed;
    DecodedAudioBuffer decoded;
    decoded.sample_rate = 48000;
    decoded.channels = 2;
    std::array<float, 5760 * 2> pcm{};
    for (;;) {
        const int frames = op_read_float_stereo(stream.get(), pcm.data(), static_cast<int>(pcm.size()));
        // Do not publish partial PCM after a decode error.
        if (frames < 0) return OpusDecodeResult::failed;
        if (frames == 0) break;
        const auto count = static_cast<size_t>(frames) * 2;
        if (!std::all_of(pcm.begin(), pcm.begin() + count,
                [](float sample) { return std::isfinite(sample); })) return OpusDecodeResult::failed;
        decoded.samples.insert(decoded.samples.end(), pcm.begin(), pcm.begin() + count);
    }
    if (decoded.samples.empty()) return OpusDecodeResult::failed;
    output = std::move(decoded);
    return OpusDecodeResult::decoded;
}

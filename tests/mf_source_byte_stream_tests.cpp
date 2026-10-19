#include "../apps/show_host/media/mf_source_byte_stream.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

class Completion final : public IMFAsyncCallback {
public:
    Completion(IMFByteStream* source, IUnknown* expected_state)
        : source(source), expected_state(expected_state) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFAsyncCallback)) return E_NOINTERFACE;
        *out = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { return --references; }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override {
        IUnknown* state{};
        expect(SUCCEEDED(result->GetState(&state)) && state == expected_state,
            "byte stream changed the caller's asynchronous state");
        if (state) state->Release();
        status = source->EndRead(result, &actual);
        ULONG duplicate{};
        expect(source->EndRead(result, &duplicate) == MF_E_INVALIDREQUEST,
            "the same byte read was completed twice");
        SetEvent(ready);
        return S_OK;
    }
    IMFByteStream* source;
    IUnknown* expected_state;
    HANDLE ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HRESULT status{E_PENDING};
    ULONG actual{};
    std::atomic<ULONG> references{1};
};
}

int main() {
    using namespace std::chrono_literals;
    MfSourceByteStreamReads reads;
    const auto now = MfSourceByteStreamReads::Time{};
    expect(!reads.buffering(now + 10s), "decode lateness with no byte I/O was called buffering");
    auto request = reads.begin(now);
    expect(!reads.buffering(now + 99ms), "ordinary byte I/O entered buffering immediately");
    expect(reads.buffering(now + 100ms), "an actual outstanding byte read was not buffering");
    reads.finish(request, false, false, true);
    expect(!reads.buffering(now + 10s), "completed byte I/O left stale buffering state");
    request = reads.begin(now);
    reads.finish(request, true, false, false);
    expect(reads.buffering(now), "a failed transport read lost its buffering evidence");
    request = reads.begin(now);
    reads.finish(request, false, true, false);
    expect(reads.premature_eof(), "truncated transport EOS was accepted as a video tail");
    const auto old_request = request;
    request = reads.begin(now);
    reads.finish(request, false, false, true);
    reads.finish(old_request, true, true, false);
    expect(!reads.buffering(now) && !reads.premature_eof(),
        "an obsolete completion poisoned a recovered source");
    request = reads.begin(now);
    reads.cancel(request);
    expect(!reads.buffering(now + 10s), "cancellation retained an outstanding read");
    const auto previous_seek_read = reads.begin(now);
    reads.reset();
    request = reads.begin(now);
    reads.finish(previous_seek_read, true, true, false);
    reads.finish(request, false, false, true);
    expect(!reads.buffering(now + 10s) && !reads.premature_eof(),
        "a late completion from an old seek poisoned the new position");
    bool bytes_in_cache = false;
    request = reads.begin(now, [&] { return bytes_in_cache; });
    expect(reads.buffering(now + 1s), "an uncached network range was not buffering");
    bytes_in_cache = true;
    expect(!reads.buffering(now + 10s),
        "a delayed callback for already-cached bytes was called buffering");
    reads.finish(request, false, false, true);

    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    expect(SUCCEEDED(MFStartup(MF_VERSION)), "MFStartup failed");
    wchar_t temp[MAX_PATH]{}, file[MAX_PATH]{};
    expect(GetTempPathW(MAX_PATH, temp) && GetTempFileNameW(temp, L"krb", 0, file),
        "cache range fixture path creation failed");
    const std::wstring cache_path = std::wstring(file) + L" \x7f13\x5b58+.mp4";
    DeleteFileW(file);
    // Match Dart's query encoding, including Unicode, '+' and spaces.
    const auto utf8_size = WideCharToMultiByte(CP_UTF8, 0, cache_path.c_str(),
        -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(utf8_size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, cache_path.c_str(), -1,
        utf8.data(), utf8_size, nullptr, nullptr);
    std::wstring encoded;
    constexpr wchar_t hex[] = L"0123456789ABCDEF";
    for (int i = 0; i + 1 < utf8_size; ++i) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        encoded += L'%'; encoded += hex[c >> 4]; encoded += hex[c & 15];
    }
    const auto loopback = L"http://127.0.0.1:12345/cache/token?kirakara-cache=" + encoded;
    auto cache = make_local_cache_completion_probe(loopback, 10);
    expect(static_cast<bool>(cache) && !cache(), "absent cache bytes were called available");
    expect(!make_local_cache_completion_probe(
        L"http://example.com/cache/token?kirakara-cache=" + encoded, 10),
        "a remote URL was allowed to access local cache metadata");
    expect(!make_local_cache_completion_probe(
        L"http://127.0.0.1:12345/cache/token?kirakara-cache=%5C%5Cserver%5Cfile", 10),
        "a UNC path was allowed to block the clock on network metadata");
    expect(!make_local_cache_completion_probe(loopback + L"%ZZ", 10), "malformed path escape was accepted");
    const auto part = cache_path + L".part";
    HANDLE cache_file = CreateFileW(part.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    expect(cache_file != INVALID_HANDLE_VALUE, "cache prefix creation failed");
    DWORD count{};
    expect(WriteFile(cache_file, "12345", 5, &count, nullptr) && count == 5,
        "cache prefix write failed");
    expect(!cache(), "a partial prefix was mistaken for a completed cache");
    expect(WriteFile(cache_file, "67890", 5, &count, nullptr) && count == 5,
        "cache completion write failed");
    expect(cache(), "a complete cache was not recognized");
    LARGE_INTEGER prefix_size{};
    prefix_size.QuadPart = 5;
    expect(SetFilePointerEx(cache_file, prefix_size, nullptr, FILE_BEGIN)
        && SetEndOfFile(cache_file), "cache retry truncation failed");
    expect(!cache(), "a truncated retry retained stale complete-cache state");
    expect(WriteFile(cache_file, "67890", 5, &count, nullptr) && count == 5,
        "cache retry completion failed");
    CloseHandle(cache_file);
    expect(MoveFileW(part.c_str(), cache_path.c_str()), "cache final rename failed");
    expect(cache(), "cache final rename lost completion state");
    DeleteFileW(cache_path.c_str());

    const std::array<BYTE, 6> data{1, 2, 3, 4, 5, 6};
    IMFByteStream* inner{};
    expect(SUCCEEDED(MFCreateTempFile(MF_ACCESSMODE_READWRITE,
        MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, &inner)),
        "temporary MF byte stream creation failed");
    ULONG written{};
    expect(SUCCEEDED(inner->Write(data.data(), data.size(), &written))
        && written == data.size() && SUCCEEDED(inner->SetCurrentPosition(0)),
        "temporary byte stream setup failed");
    auto observed_reads = std::make_shared<MfSourceByteStreamReads>();
    IMFByteStream* observed{};
    expect(SUCCEEDED(create_observed_source_byte_stream(inner, observed_reads, &observed)),
        "observed stream creation failed");
    BYTE buffer[6]{};
    ULONG actual{};
    expect(SUCCEEDED(observed->Read(buffer, 2, &actual)) && actual == 2 && buffer[1] == 2,
        "synchronous byte read changed the data");
    Completion completion(observed, inner);
    expect(SUCCEEDED(observed->BeginRead(buffer, 4, &completion, inner)), "async byte read failed to start");
    expect(WaitForSingleObject(completion.ready, 2000) == WAIT_OBJECT_0,
        "async byte read did not complete");
    expect(SUCCEEDED(completion.status) && completion.actual == 4 && buffer[0] == 3 && buffer[3] == 6,
        "async byte read lost its data or status");
    expect(SUCCEEDED(observed->Read(buffer, 1, &actual)) && actual == 0,
        "a valid file EOF was changed");
    expect(!observed_reads->buffering() && !observed_reads->premature_eof(),
        "a complete cached file was treated as a network stall");
    QWORD position{};
    expect(SUCCEEDED(observed->Seek(msoBegin, 1, 0, &position)) && position == 1,
        "byte stream seek semantics changed");
    expect(SUCCEEDED(observed->Read(buffer, 1, &actual)) && actual == 1 && buffer[0] == 2,
        "post-seek data changed");
    expect(FAILED(observed->Seek(msoBegin, -1, 0, &position))
        && !observed_reads->buffering(), "a rejected seek was called network buffering");
    observed->Close();
    observed->Release();
    inner->Release();
    CloseHandle(completion.ready);
    MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    std::puts("MF source byte stream tests passed");
}

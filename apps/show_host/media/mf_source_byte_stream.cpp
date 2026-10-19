#include "mf_source_byte_stream.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <atomic>
#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

namespace {

template<class T> void release(T*& object) {
    if (object) object->Release();
    object = nullptr;
}

bool transfer_failed(HRESULT hr) {
    // Cancellation, a rejected async request, or a failed seek do not prove
    // that the network ran out of data. Only transport failures are sticky.
    const auto value = static_cast<unsigned long>(hr);
    if (value >= 0xc00d4268UL && value <= 0xc00d4295UL) return true;
    if (HRESULT_FACILITY(hr) != FACILITY_WIN32) return false;
    const auto code = HRESULT_CODE(hr);
    if (code == 12017 || code == 10004) return false; // WinHTTP cancel / WSAEINTR
    return (code >= 10000 && code <= 12199)
        || code == ERROR_BROKEN_PIPE || code == ERROR_NETNAME_DELETED
        || code == ERROR_CONNECTION_ABORTED || code == ERROR_CONNECTION_REFUSED
        || code == ERROR_NETWORK_UNREACHABLE || code == ERROR_SEM_TIMEOUT
        || code == ERROR_TIMEOUT;
}

std::wstring local_cache_path(const std::wstring& url) {
    // Only trust metadata from the App's IPv4 loopback cache endpoint. In
    // particular, never stat a path supplied by a remote media URL or UNC share.
    constexpr std::wstring_view prefix = L"http://127.0.0.1:";
    if (!url.starts_with(prefix)) return {};
    const auto path_start = url.find(L'/', prefix.size());
    if (path_start == std::wstring::npos || path_start == prefix.size()
            || url.find_first_not_of(L"0123456789", prefix.size()) != path_start
            || url.compare(path_start, 7, L"/cache/") != 0) return {};
    const auto query = url.find(L'?', path_start);
    if (query == std::wstring::npos) return {};
    std::wstring encoded;
    for (auto start = query + 1; start < url.size();) {
        auto end = url.find_first_of(L"&#", start);
        if (end == std::wstring::npos) end = url.size();
        const auto item = url.substr(start, end - start);
        constexpr std::wstring_view key = L"kirakara-cache=";
        if (item.starts_with(key)) { encoded = item.substr(key.size()); break; }
        if (end == url.size() || url[end] == L'#') break;
        start = end + 1;
    }
    std::string bytes;
    const auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        auto c = encoded[i];
        if (c == L'%') {
            if (i + 2 >= encoded.size()) return {};
            const auto high = hex(encoded[i + 1]), low = hex(encoded[i + 2]);
            if (high < 0 || low < 0) return {};
            c = static_cast<wchar_t>(high * 16 + low);
            i += 2;
        } else if (c == L'+') c = L' ';
        if (c == 0 || c > 255) return {};
        bytes.push_back(static_cast<char>(c));
    }
    if (bytes.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring path(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        bytes.data(), static_cast<int>(bytes.size()), path.data(), size);
    if (path.size() < 3 || !((path[0] >= L'A' && path[0] <= L'Z')
            || (path[0] >= L'a' && path[0] <= L'z'))
            || path[1] != L':' || (path[2] != L'\\' && path[2] != L'/')) return {};
    const std::wstring root = path.substr(0, 2) + L"\\";
    const auto type = GetDriveTypeW(root.c_str());
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) return {};
    return path;
}

class ReadDispatch {
public:
    ReadDispatch(IMFByteStream* source, const std::wstring& url) {
        QWORD length{};
        if (SUCCEEDED(source->GetLength(&length))) {
            cache_available_ = make_local_cache_completion_probe(url, length);
        }
        status = MFAllocateWorkQueue(&queue);
    }
    ~ReadDispatch() {
        if (SUCCEEDED(status)) MFUnlockWorkQueue(queue);
    }
    bool available() const {
        return cache_available_ && cache_available_();
    }
    DWORD queue{};
    HRESULT status{E_FAIL};
private:
    std::function<bool()> cache_available_;
};

class ReadCompletion final : public IUnknown {
public:
    explicit ReadCompletion(IMFByteStream* owner) : owner(owner) { owner->AddRef(); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references_;
        if (!left) delete this;
        return left;
    }
    IMFByteStream* const owner;
    ULONG bytes{};
    std::atomic<bool> ended{};
private:
    ~ReadCompletion() { owner->Release(); }
    std::atomic<ULONG> references_{1};
};

class ReadCallback final : public IMFAsyncCallback {
public:
    static HRESULT create(IMFByteStream* owner, IMFByteStream* source,
            std::shared_ptr<MfSourceByteStreamReads> reads,
            std::shared_ptr<ReadDispatch> dispatch,
            IMFAsyncCallback* callback, IUnknown* state,
            ULONG requested_bytes, ReadCallback** out) {
        auto* value = new (std::nothrow) ReadCallback(source,
            std::move(reads), std::move(dispatch));
        if (!value) return E_OUTOFMEMORY;
        value->completion_ = new (std::nothrow) ReadCompletion(owner);
        auto hr = value->completion_ ? S_OK : E_OUTOFMEMORY;
        if (SUCCEEDED(hr)) {
            hr = MFCreateAsyncResult(value->completion_, callback, state,
                &value->result_);
        }
        if (FAILED(hr)) { value->Release(); return hr; }
        value->has_position_ = SUCCEEDED(source->GetCurrentPosition(&value->position_));
        value->has_length_ = SUCCEEDED(source->GetLength(&value->length_));
        value->requested_bytes_ = requested_bytes;
        value->request_id_ = value->reads_->begin(MfSourceByteStreamReads::Clock::now(),
            [dispatch = value->dispatch_] {
                return dispatch->available();
            });
        *out = value;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFAsyncCallback)) {
            return E_NOINTERFACE;
        }
        *out = static_cast<IMFAsyncCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD* flags, DWORD* queue) override {
        if (!flags || !queue) return E_POINTER;
        *flags = 0;
        *queue = dispatch_->queue;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override {
        ULONG bytes{};
        const auto hr = source_->EndRead(result, &bytes);
        const bool premature = SUCCEEDED(hr) && !bytes && requested_bytes_
            && has_position_ && has_length_ && position_ < length_;
        reads_->finish(request_id_, transfer_failed(hr), premature, bytes != 0);
        completion_->bytes = bytes;
        result_->SetStatus(hr);
        return MFInvokeCallback(result_);
    }
    void reject(HRESULT hr) { reads_->finish(request_id_, transfer_failed(hr), false, false); }

private:
    ReadCallback(IMFByteStream* source,
            std::shared_ptr<MfSourceByteStreamReads> reads,
            std::shared_ptr<ReadDispatch> dispatch)
        : source_(source), reads_(std::move(reads)), dispatch_(std::move(dispatch)) {
        source_->AddRef();
    }
    ~ReadCallback() {
        if (request_id_) reads_->cancel(request_id_);
        release(result_);
        release(completion_);
        release(source_);
    }
    std::atomic<ULONG> references_{1};
    IMFByteStream* source_{};
    std::shared_ptr<MfSourceByteStreamReads> reads_;
    std::shared_ptr<ReadDispatch> dispatch_;
    IMFAsyncResult* result_{};
    ReadCompletion* completion_{};
    std::uint64_t request_id_{};
    QWORD position_{};
    QWORD length_{};
    ULONG requested_bytes_{};
    bool has_position_{};
    bool has_length_{};
};

class ObservedByteStream final : public IMFByteStream, public IMFAttributes {
public:
    ObservedByteStream(IMFByteStream* source,
            std::shared_ptr<MfSourceByteStreamReads> reads,
            std::shared_ptr<ReadDispatch> dispatch)
        : source_(source), reads_(std::move(reads)), dispatch_(std::move(dispatch)) {
        source_->AddRef();
        source_->QueryInterface(IID_PPV_ARGS(&attributes_));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IMFByteStream)) {
            *out = static_cast<IMFByteStream*>(this);
        } else if (iid == __uuidof(IMFAttributes) && attributes_) {
            *out = static_cast<IMFAttributes*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references_;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetCapabilities(DWORD* value) override {
        return source_->GetCapabilities(value);
    }
    HRESULT STDMETHODCALLTYPE GetLength(QWORD* value) override { return source_->GetLength(value); }
    HRESULT STDMETHODCALLTYPE SetLength(QWORD) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE GetCurrentPosition(QWORD* value) override {
        return source_->GetCurrentPosition(value);
    }
    HRESULT STDMETHODCALLTYPE SetCurrentPosition(QWORD position) override {
        reads_->reset();
        return source_->SetCurrentPosition(position);
    }
    HRESULT STDMETHODCALLTYPE IsEndOfStream(BOOL* value) override {
        return source_->IsEndOfStream(value);
    }
    HRESULT STDMETHODCALLTYPE Read(BYTE* buffer, ULONG count, ULONG* actual) override {
        if (!actual) return E_POINTER;
        *actual = 0;
        QWORD position{}, length{};
        const bool known = SUCCEEDED(source_->GetCurrentPosition(&position))
            && SUCCEEDED(source_->GetLength(&length));
        const auto id = reads_->begin(MfSourceByteStreamReads::Clock::now(),
            [dispatch = dispatch_] { return dispatch->available(); });
        const auto hr = source_->Read(buffer, count, actual);
        const bool premature = SUCCEEDED(hr) && count && !*actual
            && known && position < length;
        reads_->finish(id, transfer_failed(hr), premature, SUCCEEDED(hr) && *actual != 0);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE BeginRead(BYTE* buffer, ULONG count,
            IMFAsyncCallback* callback, IUnknown* state) override {
        if (!callback) return E_INVALIDARG;
        ReadCallback* observer{};
        auto hr = ReadCallback::create(this, source_, reads_, dispatch_, callback, state,
            count, &observer);
        if (FAILED(hr)) return hr;
        hr = source_->BeginRead(buffer, count, observer, nullptr);
        if (FAILED(hr)) observer->reject(hr);
        observer->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE EndRead(IMFAsyncResult* result, ULONG* actual) override {
        if (!result || !actual) return E_POINTER;
        IUnknown* object{};
        const auto hr = result->GetObject(&object);
        if (FAILED(hr)) return hr;
        auto* completion = dynamic_cast<ReadCompletion*>(object);
        if (!completion || completion->owner != this) {
            release(object);
            return E_INVALIDARG;
        }
        if (completion->ended.exchange(true)) {
            release(object);
            return MF_E_INVALIDREQUEST;
        }
        *actual = completion->bytes;
        release(object);
        return result->GetStatus();
    }
    HRESULT STDMETHODCALLTYPE Write(const BYTE*, ULONG, ULONG*) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE EndWrite(IMFAsyncResult*, ULONG*) override { return E_ACCESSDENIED; }
    HRESULT STDMETHODCALLTYPE Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset,
            DWORD flags, QWORD* position) override {
        reads_->reset();
        return source_->Seek(origin, offset, flags, position);
    }
    HRESULT STDMETHODCALLTYPE Flush() override { return source_->Flush(); }
    HRESULT STDMETHODCALLTYPE Close() override { reads_->reset(); return source_->Close(); }

    // Keep IUnknown identity stable when the MP4 resolver queries stream
    // attributes (MIME type/original URL), rather than handing out an inner
    // IMFAttributes that can lead it back to the unobserved byte stream.
#define FORWARD_ATTRIBUTE(method, arguments, values) \
    HRESULT STDMETHODCALLTYPE method arguments override { return attributes_->method values; }
    FORWARD_ATTRIBUTE(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
    FORWARD_ATTRIBUTE(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* v), (k, v))
    FORWARD_ATTRIBUTE(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* b), (k, v, b))
    FORWARD_ATTRIBUTE(Compare, (IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* b), (a, t, b))
    FORWARD_ATTRIBUTE(GetUINT32, (REFGUID k, UINT32* v), (k, v))
    FORWARD_ATTRIBUTE(GetUINT64, (REFGUID k, UINT64* v), (k, v))
    FORWARD_ATTRIBUTE(GetDouble, (REFGUID k, double* v), (k, v))
    FORWARD_ATTRIBUTE(GetGUID, (REFGUID k, GUID* v), (k, v))
    FORWARD_ATTRIBUTE(GetStringLength, (REFGUID k, UINT32* v), (k, v))
    FORWARD_ATTRIBUTE(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32* a), (k, v, n, a))
    FORWARD_ATTRIBUTE(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* n), (k, v, n))
    FORWARD_ATTRIBUTE(GetBlobSize, (REFGUID k, UINT32* n), (k, n))
    FORWARD_ATTRIBUTE(GetBlob, (REFGUID k, UINT8* v, UINT32 n, UINT32* a), (k, v, n, a))
    FORWARD_ATTRIBUTE(GetAllocatedBlob, (REFGUID k, UINT8** v, UINT32* n), (k, v, n))
    FORWARD_ATTRIBUTE(GetUnknown, (REFGUID k, REFIID iid, void** v), (k, iid, v))
    FORWARD_ATTRIBUTE(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
    FORWARD_ATTRIBUTE(DeleteItem, (REFGUID k), (k))
    FORWARD_ATTRIBUTE(DeleteAllItems, (), ())
    FORWARD_ATTRIBUTE(SetUINT32, (REFGUID k, UINT32 v), (k, v))
    FORWARD_ATTRIBUTE(SetUINT64, (REFGUID k, UINT64 v), (k, v))
    FORWARD_ATTRIBUTE(SetDouble, (REFGUID k, double v), (k, v))
    FORWARD_ATTRIBUTE(SetGUID, (REFGUID k, REFGUID v), (k, v))
    FORWARD_ATTRIBUTE(SetString, (REFGUID k, LPCWSTR v), (k, v))
    FORWARD_ATTRIBUTE(SetBlob, (REFGUID k, const UINT8* v, UINT32 n), (k, v, n))
    FORWARD_ATTRIBUTE(SetUnknown, (REFGUID k, IUnknown* v), (k, v))
    FORWARD_ATTRIBUTE(LockStore, (), ())
    FORWARD_ATTRIBUTE(UnlockStore, (), ())
    FORWARD_ATTRIBUTE(GetCount, (UINT32* n), (n))
    FORWARD_ATTRIBUTE(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
    FORWARD_ATTRIBUTE(CopyAllItems, (IMFAttributes* a), (a))
#undef FORWARD_ATTRIBUTE

private:
    ~ObservedByteStream() { release(attributes_); release(source_); }
    std::atomic<ULONG> references_{1};
    IMFByteStream* source_{};
    IMFAttributes* attributes_{};
    std::shared_ptr<MfSourceByteStreamReads> reads_;
    std::shared_ptr<ReadDispatch> dispatch_;
};

} // namespace

std::function<bool()> make_local_cache_completion_probe(
        const std::wstring& source_url, QWORD source_length) {
    auto path = local_cache_path(source_url);
    if (path.empty() || !source_length) return {};
    return [path = std::move(path), source_length] {
        QWORD downloaded{};
        for (const auto& name : {path, path + L".part"}) {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (GetFileAttributesExW(name.c_str(), GetFileExInfoStandard, &data)
                    && !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                downloaded = std::max(downloaded,
                    (static_cast<QWORD>(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
            }
        }
        // Do not latch a .part file's size: a failed attempt can truncate it
        // before retrying. This is queried only for an outstanding slow read,
        // not on every frame of normal playback.
        return downloaded >= source_length;
    };
}

HRESULT create_observed_source_byte_stream(IMFByteStream* source,
        std::shared_ptr<MfSourceByteStreamReads> reads, IMFByteStream** result,
        const std::wstring& source_url) {
    if (!source || !reads || !result) return E_POINTER;
    *result = nullptr;
    const auto dispatch = std::make_shared<ReadDispatch>(source, source_url);
    if (FAILED(dispatch->status)) return dispatch->status;
    *result = new (std::nothrow) ObservedByteStream(source, std::move(reads), dispatch);
    return *result ? S_OK : E_OUTOFMEMORY;
}

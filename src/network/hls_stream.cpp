#include "hls_stream.h"
#include "../util/com_ptr.h"
#include "../util/log.h"
#include <winhttp.h>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <memory>

struct WinHttpHandle {
    HINTERNET h = nullptr;
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h_) : h(h_) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        std::swap(h, o.h);
        return *this;
    }
    void reset(HINTERNET h_ = nullptr) { if (h) WinHttpCloseHandle(h); h = h_; }
    HINTERNET* operator&() { return &h; }
    operator HINTERNET() const { return h; }
    explicit operator bool() const { return h != nullptr; }
};

#pragma comment(lib, "winhttp.lib")

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &w[0], len);
    return w;
}

// ========================================================================
// HlsByteStream
// ========================================================================

/*
 * Custom IMFByteStream that concatenates HLS TS segments on-the-fly.
 *
 * Key design points:
 *   - Read/seek operate over a sorted vector of LoadedSeg (segments appended
 *     by download thread via AddSegment). Binary search maps global offset
 *     to segment+offset.
 *   - Async reads (BeginRead/EndRead): if data is available, complete
 *     immediately via MFInvokeCallback. If not, queue a PendingRead and
 *     return E_PENDING — MF treats this as "retry later". When AddSegment
 *     arrives, FulfillPendingReads re-checks and completes any satisfied reads.
 *   - EOS protocol: Read() returns S_OK with 0 bytes when data is absent but
 *     EOS is not set (MF treats 0-bytes-S_OK as "try again"). Only returns
 *     S_FALSE when has_eos_marker_ is set AND position >= total_bytes_.
 *     This prevents premature EOF signaling before all segments arrive.
 *   - needs_wake_: set by AddSegment when new data may restart a stalled
 *     pipeline. The player's CheckAudio/VideoTick timers poll this flag.
 */
HlsByteStream::HlsByteStream() {
    InitializeCriticalSection(&lock_);
}

HlsByteStream::~HlsByteStream() {
    Clear();
    DeleteCriticalSection(&lock_);
}

STDMETHODIMP HlsByteStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFByteStream) {
        *ppv = static_cast<IMFByteStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) HlsByteStream::AddRef() {
    return (ULONG)ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) HlsByteStream::Release() {
    LONG r = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP HlsByteStream::GetCapabilities(DWORD* pdwCapabilities) {
    if (!pdwCapabilities) return E_POINTER;
    *pdwCapabilities = MFBYTESTREAM_IS_READABLE
                     | MFBYTESTREAM_IS_SEEKABLE
                     | MFBYTESTREAM_IS_PARTIALLY_DOWNLOADED;
    return S_OK;
}

STDMETHODIMP HlsByteStream::GetLength(QWORD* pqwLength) {
    if (!pqwLength) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pqwLength = total_bytes_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP HlsByteStream::SetLength(QWORD) {
    return E_NOTIMPL;
}

STDMETHODIMP HlsByteStream::GetCurrentPosition(QWORD* pqwPosition) {
    if (!pqwPosition) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pqwPosition = read_pos_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

void HlsByteStream::CancelPendingReadsLocked() {
    std::vector<PendingRead> to_cancel;
    to_cancel.swap(pending_reads_);

    for (auto& pr : to_cancel) {
        async_result_ = 0;
        async_hr_ = E_ABORT;
        LeaveCriticalSection(&lock_);

        ComPtr<IMFAsyncResult> result;
        if (SUCCEEDED(MFCreateAsyncResult(nullptr, pr.cb, pr.state, &result))) {
            MFInvokeCallback(result.get());
        } else {
            LOG_ERROR("MFCreateAsyncResult failed in CancelPendingReadsLocked");
        }
        pr.cb->Release();
        if (pr.state) pr.state->Release();

        EnterCriticalSection(&lock_);
    }
}

STDMETHODIMP HlsByteStream::SetCurrentPosition(QWORD qwPosition) {
    EnterCriticalSection(&lock_);
    CancelPendingReadsLocked();
    read_pos_ = (int64_t)qwPosition;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP HlsByteStream::IsEndOfStream(BOOL* pfEndOfStream) {
    if (!pfEndOfStream) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pfEndOfStream = (has_eos_marker_ && read_pos_ >= total_bytes_) ? TRUE : FALSE;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

ULONG HlsByteStream::CopyFromSegmentsLocked(BYTE* pb, ULONG cb) {
    ULONG total_read = 0;
    while (cb > 0) {
        int seg_idx = -1;
        int64_t offset_in_seg = 0;
        int lo = 0, hi = (int)segs_.size() - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            auto& s = segs_[mid];
            if (read_pos_ >= s.offset && read_pos_ < s.offset + (int64_t)s.size) {
                seg_idx = mid;
                offset_in_seg = read_pos_ - s.offset;
                break;
            }
            if (read_pos_ < s.offset) hi = mid - 1;
            else lo = mid + 1;
        }
        if (seg_idx >= 0 && segs_[seg_idx].data) {
            LoadedSeg& seg = segs_[seg_idx];
            size_t to_copy = seg.size - (size_t)offset_in_seg;
            if (to_copy > cb) to_copy = cb;
            memcpy(pb, seg.data + offset_in_seg, to_copy);
            total_read += (ULONG)to_copy;
            cb -= (ULONG)to_copy;
            pb += to_copy;
            read_pos_ += to_copy;
            // Free fully consumed segment data to prevent unbounded growth in live streams.
            // For VOD (cache_data_=true) we keep data so seek-back can re-read it.
            if (!cache_data_ && offset_in_seg + (int64_t)to_copy >= (int64_t)seg.size) {
                free(seg.data);
                seg.data = nullptr;
            }
        } else {
            break;
        }
    }
    return total_read;
}



/* Sync read: copy from segments under lock. Returns S_OK even with 0 bytes
 * when data is absent but EOS not signaled — MF will retry
 * (S_FALSE would trigger MF_SOURCE_READERF_ENDOFSTREAM). */
STDMETHODIMP HlsByteStream::Read(BYTE* pb, ULONG cb, ULONG* pcbRead) {
    if (!pb) return E_POINTER;

    EnterCriticalSection(&lock_);
    if (closed_) { LeaveCriticalSection(&lock_); return E_ABORT; }
    ULONG total_read = CopyFromSegmentsLocked(pb, cb);

    // If nothing read and EOS marker set, signal end
    if (total_read == 0 && has_eos_marker_ && read_pos_ >= total_bytes_) {
        LeaveCriticalSection(&lock_);
        if (pcbRead) *pcbRead = 0;
        return S_FALSE;
    }

    LeaveCriticalSection(&lock_);
    if (pcbRead) *pcbRead = total_read;
    if (total_read > 0) return S_OK;
    // No data available but no EOS — not end of stream, caller should retry
    return S_OK;
}

/* Async read: if data (or EOS) available, complete immediately on this thread.
 * Otherwise queue as PENDING — FulfillPendingReads() will satisfy it when
 * AddSegment delivers new data. MF serializes BeginRead calls (only one
 * outstanding per byte stream), so async_result_/async_hr_ are safe. */
STDMETHODIMP HlsByteStream::BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* pState) {
    if (!pb || !pCallback) return E_POINTER;

    EnterCriticalSection(&lock_);
    if (closed_) { LeaveCriticalSection(&lock_); return E_ABORT; }
    ULONG bytesRead = CopyFromSegmentsLocked(pb, cb);
    bool is_eos = (bytesRead == 0 && has_eos_marker_ && read_pos_ >= total_bytes_);

    if (bytesRead > 0 || is_eos) {
        async_result_.store(bytesRead, std::memory_order_release);
        async_hr_.store(is_eos ? S_FALSE : S_OK, std::memory_order_release);
        LeaveCriticalSection(&lock_);

        pCallback->AddRef();
        if (pState) pState->AddRef();
        ComPtr<IMFAsyncResult> result;
        HRESULT hr = MFCreateAsyncResult(nullptr, pCallback, pState, &result);
        pCallback->Release();
        if (pState) pState->Release();
        if (FAILED(hr)) return hr;
        return MFInvokeCallback(result.get());
    }

    PendingRead pr;
    pr.buf = pb;
    pr.size = cb;
    pr.cb = pCallback;
    pr.state = pState;
    pCallback->AddRef();
    if (pState) pState->AddRef();
    pending_reads_.push_back(pr);
    LeaveCriticalSection(&lock_);
    return E_PENDING;
}

STDMETHODIMP HlsByteStream::EndRead(IMFAsyncResult* /*pResult*/, ULONG* pcbRead) {
    if (pcbRead) *pcbRead = async_result_.load(std::memory_order_acquire);
    return async_hr_.load(std::memory_order_acquire);
}

STDMETHODIMP HlsByteStream::Write(const BYTE*, ULONG, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP HlsByteStream::BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) { return E_NOTIMPL; }
STDMETHODIMP HlsByteStream::EndWrite(IMFAsyncResult*, ULONG*) { return E_NOTIMPL; }

STDMETHODIMP HlsByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset,
                                  DWORD /*dwSeekFlags*/, QWORD* pqwCurrentPosition) {
    EnterCriticalSection(&lock_);
    CancelPendingReadsLocked();
    if (SeekOrigin == 0) {  // MFBYTESTREAM_SEEK_ORIGIN_Begin
        read_pos_ = llSeekOffset;
    } else {
        read_pos_ = read_pos_ + llSeekOffset;
    }
    if (read_pos_ < 0) read_pos_ = 0;
    int64_t tb = total_bytes_.load(std::memory_order_acquire);
    if (read_pos_ > tb) read_pos_ = tb;
    if (pqwCurrentPosition) *pqwCurrentPosition = read_pos_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP HlsByteStream::Flush() { return S_OK; }

STDMETHODIMP HlsByteStream::Close() {
    EnterCriticalSection(&lock_);
    CancelPendingReadsLocked();
    closed_ = true;
    for (auto& s : segs_) {
        free(s.data);
        s.data = nullptr;
    }
    segs_.clear();
    total_bytes_ = 0;
    read_pos_ = 0;
    has_eos_marker_ = false;
    end_of_stream_ = false;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

void HlsByteStream::AddSegment(const uint8_t* data, size_t size) {
    EnterCriticalSection(&lock_);

    LoadedSeg seg;
    seg.data = (uint8_t*)malloc(size);
    if (!seg.data) { LeaveCriticalSection(&lock_); return; }
    memcpy(seg.data, data, size);
    seg.size = size;
    seg.offset = total_bytes_;
    segs_.push_back(seg);

    total_bytes_ += size;

    needs_wake_.store(1, std::memory_order_release);

    FulfillPendingReads();
}

/* Called by AddSegment (download thread) to complete any pending async reads
 * that can now be satisfied. Reads that still have no data are re-queued.
 * Lock is released before MFInvokeCallback to avoid reentrancy deadlocks. */
void HlsByteStream::FulfillPendingReads() {
    std::vector<PendingRead> to_fulfill;
    to_fulfill.swap(pending_reads_);

    for (auto& pr : to_fulfill) {
        ULONG n = CopyFromSegmentsLocked(pr.buf, pr.size);
        bool is_eos = (n == 0 && has_eos_marker_ && read_pos_ >= total_bytes_);

        if (n == 0 && !is_eos) {
            pending_reads_.push_back(pr);
            continue;
        }

        async_result_.store(n, std::memory_order_release);
        async_hr_.store(is_eos ? S_FALSE : S_OK, std::memory_order_release);
        LeaveCriticalSection(&lock_);

        ComPtr<IMFAsyncResult> result;
        if (SUCCEEDED(MFCreateAsyncResult(nullptr, pr.cb, pr.state, &result))) {
            MFInvokeCallback(result.get());
        } else {
            LOG_ERROR("MFCreateAsyncResult failed in FulfillPendingReads");
        }
        pr.cb->Release();
        if (pr.state) pr.state->Release();

        EnterCriticalSection(&lock_);
    }
}

bool HlsByteStream::CheckAndClearNeedsWake() {
    return needs_wake_.exchange(0, std::memory_order_acq_rel) != 0;
}

bool HlsByteStream::HasUnreadData() const {
    return read_pos_.load(std::memory_order_acquire) < total_bytes_.load(std::memory_order_acquire);
}

void HlsByteStream::Clear() {
    EnterCriticalSection(&lock_);
    CancelPendingReadsLocked();
    for (auto& s : segs_) {
        free(s.data);
        s.data = nullptr;
    }
    segs_.clear();
    total_bytes_ = 0;
    read_pos_ = 0;
    has_eos_marker_ = false;
    end_of_stream_ = false;
    LeaveCriticalSection(&lock_);
}

void HlsByteStream::SetEndOfStream() {
    EnterCriticalSection(&lock_);
    has_eos_marker_ = true;

    std::vector<PendingRead> to_fulfill;
    to_fulfill.swap(pending_reads_);

    for (auto& pr : to_fulfill) {
        async_result_.store(0, std::memory_order_release);
        async_hr_.store(S_FALSE, std::memory_order_release);
        LeaveCriticalSection(&lock_);

        ComPtr<IMFAsyncResult> result;
        if (SUCCEEDED(MFCreateAsyncResult(nullptr, pr.cb, pr.state, &result))) {
            MFInvokeCallback(result.get());
        } else {
            LOG_ERROR("MFCreateAsyncResult failed in SetEndOfStream");
        }
        pr.cb->Release();
        if (pr.state) pr.state->Release();

        EnterCriticalSection(&lock_);
    }

    LeaveCriticalSection(&lock_);
}

// ========================================================================
// HlsManager
// ========================================================================

HlsManager::HlsManager() {
    InitializeCriticalSection(&seg_lock_);
    wake_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

HlsManager::~HlsManager() {
    Close();
    DeleteCriticalSection(&seg_lock_);
    if (wake_event_) CloseHandle(wake_event_);
}

bool HlsManager::DownloadUrl(const wchar_t* url, std::vector<uint8_t>& out) {
    // Manual URL parsing (avoid WinHttpCrackUrl issues with long/encoded URLs)
    std::wstring url_str(url);
    size_t prot_end = url_str.find(L"://");
    if (prot_end == std::wstring::npos) return false;
    bool is_https = (_wcsnicmp(url, L"https", 5) == 0);

    size_t host_start = prot_end + 3;
    size_t host_end = url_str.find(L'/', host_start);
    if (host_end == std::wstring::npos) host_end = url_str.length();

    std::wstring host = url_str.substr(host_start, host_end - host_start);
    std::wstring path = url_str.substr(host_end);
    if (path.empty()) path = L"/";

    INTERNET_PORT port = is_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = host.find(L':');
    if (colon != std::wstring::npos) {
        port = (INTERNET_PORT)_wtoi(host.substr(colon + 1).c_str());
        host = host.substr(0, colon);
    }

    WinHttpHandle hSession(WinHttpOpen(L"MinPlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0));
    if (!hSession) { LOG_ERROR("WinHttpOpen failed: %u", GetLastError()); return false; }

    WinHttpHandle hConnect(WinHttpConnect(hSession, host.c_str(), port, 0));
    if (!hConnect) return false;

    DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE;
    if (is_https) flags |= WINHTTP_FLAG_SECURE;

    WinHttpHandle hRequest(WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
        nullptr, nullptr, flags));
    if (!hRequest) return false;

    // Security: restrict TLS to 1.2 and 1.3 (no SSLv2/3, no TLS 1.0/1.1)
    DWORD tls_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
                        | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURE_PROTOCOLS,
                     &tls_protocols, sizeof(tls_protocols));

    // Enable certificate revocation checking (OCSP/CRL)
    DWORD feature = WINHTTP_ENABLE_SSL_REVOCATION;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_ENABLE_FEATURE,
                     &feature, sizeof(feature));

    // Follow HTTP redirects (301/302)
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));

    // Set timeout
    WinHttpSetTimeouts(hRequest, 5000, 5000, 10000, 10000);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr))
        return false;

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        nullptr, &status_code, &status_size, nullptr);

    if (status_code != 200) {
        LOG_WARN("HTTP %lu for %ws", status_code, url);
        return false;
    }

    out.clear();
    uint8_t buf[65536];
    DWORD bytes_read = 0;
    while (download_running_ &&
           WinHttpReadData(hRequest, buf, sizeof(buf), &bytes_read) && bytes_read > 0)
        out.insert(out.end(), buf, buf + bytes_read);

    if (!download_running_) return false;
    return true;
}

std::wstring HlsManager::ResolveUrl(const std::wstring& base, const std::wstring& relative) {
    if (relative.find(L"://") != std::wstring::npos)
        return relative;

    // Strip query string from base URL
    std::wstring clean(base, 0, base.find(L'?'));

    if (!relative.empty() && relative[0] == L'/') {
        // Absolute path — replace path after host
        size_t p = clean.find(L"://");
        if (p != std::wstring::npos) {
            p = clean.find(L'/', p + 3);
            if (p != std::wstring::npos)
                return clean.substr(0, p) + relative;
        }
        return clean + relative;
    }

    // Base directory
    size_t slash = clean.rfind(L'/');
    std::wstring result = (slash != std::wstring::npos)
        ? clean.substr(0, slash + 1) + relative
        : clean + L"/" + relative;

    // Normalize ./ and ../
    for (;;) {
        size_t dotdot = result.find(L"/../");
        if (dotdot == std::wstring::npos) break;
        size_t prev = result.rfind(L'/', dotdot - 1);
        if (prev == std::wstring::npos || prev == 0) {
            result.erase(0, dotdot + 4);
        } else {
            result.erase(prev + 1, dotdot - prev + 3);
        }
    }
    if (result.size() >= 3 && result.substr(result.size() - 3) == L"/..") {
        size_t prev = result.rfind(L'/', result.size() - 4);
        if (prev == std::wstring::npos) prev = 0;
        result.erase(prev);
    }
    for (size_t p = result.find(L"/./"); p != std::wstring::npos; p = result.find(L"/./", p))
        result.erase(p, 2);

    return result;
}

bool HlsManager::ParseMasterPlaylist(const std::string& content, const std::wstring& base_url) {
    std::istringstream stream(content);
    std::string line;
    int best_bw = -1;
    std::string best_url_str;
    std::string stream_info;

    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;

        if (line.substr(0, 18) == "#EXT-X-STREAM-INF:") {
            stream_info = line.substr(18);
        } else if (!line.empty() && line[0] != '#') {
            if (!stream_info.empty()) {
                auto pos = stream_info.find("BANDWIDTH=");
                int bw = 0;
                if (pos != std::string::npos) {
                    std::string val = stream_info.substr(pos + 10);
                    auto end = val.find_first_of(", \r\n");
                    if (end != std::string::npos) val = val.substr(0, end);
                    try { bw = std::stoi(val); } catch (...) { bw = 0; }
                }
                if (bw > best_bw) {
                    best_bw = bw;
                    best_url_str = line;
                }
                stream_info.clear();
            }
        }
    }

    if (best_url_str.empty()) return false;

    std::wstring variant_url = ResolveUrl(base_url,
        Utf8ToWide(best_url_str));

    std::vector<uint8_t> data;
    if (!DownloadUrl(variant_url.c_str(), data)) {
        LOG_ERROR("Failed to download variant playlist");
        return false;
    }
    return ParseMediaPlaylist(
        std::string((char*)data.data(), data.size()), variant_url);
}

bool HlsManager::ParseMediaPlaylist(const std::string& content, const std::wstring& base_url) {
    media_playlist_url_ = base_url;
    std::istringstream stream(content);
    std::string line;
    double current_duration = 0;

    is_live_ = true; // default — #EXT-X-ENDLIST will set to false

    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty()) continue;

        if (line.substr(0, 8) == "#EXTINF:") {
            std::string val = line.substr(8);
            auto comma = val.find(',');
            if (comma != std::string::npos)
                val = val.substr(0, comma);
            try { current_duration = std::stod(val); } catch (...) { current_duration = 0; }
        } else if (line.substr(0, 22) == "#EXT-X-TARGETDURATION:") {
            try { target_duration_ = std::stoi(line.substr(22)); } catch (...) { target_duration_ = 0; }
        } else if (line.substr(0, 22) == "#EXT-X-MEDIA-SEQUENCE:") {
            try { media_sequence_ = std::stoi(line.substr(22)); } catch (...) { media_sequence_ = 0; }
        } else if (line == "#EXT-X-ENDLIST") {
            is_live_ = false;
        } else if (line.substr(0, 11) == "#EXT-X-KEY:") {
            LOG_ERROR("HLS: encrypted streams require EXT-X-KEY support");
            return false;
        } else if (!line.empty() && line[0] != '#') {
            HlsSegment seg;
            seg.url = ResolveUrl(base_url,
                Utf8ToWide(line));
            seg.duration = current_duration > 0 ? current_duration : (double)target_duration_;
            seg.byte_offset = 0;
            seg.byte_size = -1;
            segments_.push_back(seg);
            current_duration = 0;
        }
    }

    { double d = 0; for (auto& s : segments_) d += s.duration; duration_.store(d, std::memory_order_relaxed); }

    LOG_INFO("HLS: %d segments, %.1f s, %s",
             (int)segments_.size(), duration_.load(std::memory_order_relaxed), is_live_.load(std::memory_order_relaxed) ? "LIVE" : "VOD");
    return !segments_.empty();
}

bool HlsManager::Open(const wchar_t* url) {
    playlist_url_ = url;
    base_url_ = url;
    size_t last_slash = std::wstring(url).rfind(L'/');
    if (last_slash != std::wstring::npos)
        base_url_ = std::wstring(url, 0, last_slash + 1);

    LOG_INFO("HLS: Opening %ws", url);

    std::vector<uint8_t> content;
    if (!DownloadUrl(url, content)) {
        LOG_ERROR("HLS: Failed to download playlist");
        return false;
    }

    std::string content_str((char*)content.data(), content.size());

    // Check for #EXTM3U
    if (content_str.find("#EXTM3U") == std::string::npos) {
        LOG_ERROR("HLS: Not a valid m3u8 playlist");
        return false;
    }

    // Check if it's a master playlist (has #EXT-X-STREAM-INF) or media
    if (content_str.find("#EXT-X-STREAM-INF:") != std::string::npos) {
        LOG_INFO("HLS: Master playlist detected");
        if (!ParseMasterPlaylist(content_str, base_url_)) {
            LOG_ERROR("HLS: Failed to parse master playlist");
            return false;
        }
    } else {
        if (!ParseMediaPlaylist(content_str, base_url_)) {
            LOG_ERROR("HLS: Failed to parse media playlist");
            return false;
        }
    }

    if (segments_.empty()) {
        LOG_ERROR("HLS: No segments found");
        return false;
    }

    LOG_INFO("HLS: First segment: %ws", segments_[0].url.c_str());

    // Create byte stream
    byte_stream_ = new (std::nothrow) HlsByteStream();
    if (!byte_stream_) {
        LOG_CRITICAL("Out of memory: HlsByteStream");
        return false;
    }
    byte_stream_->SetCacheData(!is_live_);  // VOD: keep data for seek-back

    // Pre-buffer first 3 segments
    int prebuffer = (std::min)(3, (int)segments_.size());
    for (int i = 0; i < prebuffer; i++) {
        std::vector<uint8_t> seg_data;
        LOG_INFO("HLS: Pre-buffering segment %d/%d", i + 1, (int)segments_.size());
        if (!DownloadUrl(segments_[i].url.c_str(), seg_data)) {
            LOG_ERROR("HLS: Failed to download segment %d", i);
            continue;
        }
        byte_stream_->AddSegment(seg_data.data(), seg_data.size());
        consumed_up_to_ = i + 1;
        LOG_INFO("HLS: Segment %d: %zu bytes", i + 1, seg_data.size());
    }

    // Start background download thread for remaining segments
    next_segment_to_download_ = prebuffer;
    download_running_ = true;
    download_thread_ = CreateThread(nullptr, 0,
        [](LPVOID arg) -> DWORD {
            ((HlsManager*)arg)->DownloadLoop();
            return 0;
        }, this, 0, nullptr);
    if (!download_thread_) {
        LOG_CRITICAL("Failed to create download thread");
        download_running_ = false;
        return false;
    }

    return true;
}

void HlsManager::ReloadPlaylist() {
    // Download and parse current media playlist
    std::vector<uint8_t> content;
    if (!DownloadUrl(media_playlist_url_.c_str(), content)) {
        LOG_WARN("HLS: Playlist reload failed");
        return;
    }

    std::string content_str((char*)content.data(), content.size());
    std::vector<HlsSegment> parsed;
    double current_duration = 0;
    {
        std::istringstream stream(content_str);
        std::string line;
        while (std::getline(stream, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty()) continue;
            if (line.substr(0, 8) == "#EXTINF:") {
                std::string val = line.substr(8);
                auto comma = val.find(',');
                if (comma != std::string::npos) val = val.substr(0, comma);
                try { current_duration = std::stod(val); } catch (...) { current_duration = 0; }
            } else if (line.substr(0, 22) == "#EXT-X-MEDIA-SEQUENCE:") {
                try { media_sequence_ = std::stoi(line.substr(22)); } catch (...) { media_sequence_ = 0; }
            } else if (line == "#EXT-X-ENDLIST") {
                is_live_ = false;
            } else if (!line.empty() && line[0] != '#') {
                HlsSegment seg;
                seg.url = ResolveUrl(base_url_, Utf8ToWide(line));
                seg.duration = current_duration > 0 ? current_duration : (double)target_duration_;
                parsed.push_back(seg);
                current_duration = 0;
            }
        }
    }

    if (parsed.empty()) return;

    // Add only truly new segments (by URL comparison)
    int added = 0;
    for (auto& ns : parsed) {
        bool found = false;
        EnterCriticalSection(&seg_lock_);
        for (auto& es : segments_) {
            if (es.url == ns.url) { found = true; break; }
        }
        LeaveCriticalSection(&seg_lock_);
        if (found) continue;

        // Download before adding to segments_ — don't record failures permanently
        std::vector<uint8_t> seg_data;
        if (!DownloadUrl(ns.url.c_str(), seg_data)) {
            LOG_WARN("HLS: Failed to download new segment from reload");
            continue;
        }

        EnterCriticalSection(&seg_lock_);
        segments_.push_back(ns);
        LeaveCriticalSection(&seg_lock_);
        duration_.store(duration_.load(std::memory_order_relaxed) + ns.duration, std::memory_order_release);

        byte_stream_->AddSegment(seg_data.data(), seg_data.size());
        added++;
    }

    if (added > 0) {
        LOG_INFO("HLS: Reload added %d new segments", added);
        EnterCriticalSection(&seg_lock_);
        int new_size = (int)segments_.size();
        LeaveCriticalSection(&seg_lock_);
        next_segment_to_download_.store(new_size, std::memory_order_release);
        consumed_up_to_ = new_size;
    }
}

/*
 * Background download thread loop.
 *
 * For VOD: downloads all segments sequentially, then sets EOS marker.
 * For live: after known segments exhausted, calls ReloadPlaylist() to fetch
 * new segments from server. Waits target_duration_ between reloads to avoid
 * hammering the server.
 *
 * Error handling: 3 retries per segment with 1s delay, then skip.
 * Pre-buffered segments (first 3 in Open) skip download.
 */
void HlsManager::DownloadLoop() {
    int retry_count = 0;
    while (download_running_) {
        int idx = next_segment_to_download_.load(std::memory_order_acquire);

        std::wstring url;
        {
            EnterCriticalSection(&seg_lock_);
            bool has_more = idx < (int)segments_.size();
            if (has_more) url = segments_[idx].url;
            LeaveCriticalSection(&seg_lock_);
            if (!has_more) {
                if (!is_live_) {
                    if (!eos_sent_) {
                        byte_stream_->SetEndOfStream();
                        eos_sent_ = true;
                    }
                    WaitForSingleObject(wake_event_, 1000);
                    continue;
                }
                // Live — reload playlist and wait for new segments
                ReloadPlaylist();
                WaitForSingleObject(wake_event_, (DWORD)(target_duration_ * 1000));
                continue;
            }
        }

        // Check if already downloaded (pre-buffered or previous loop iteration)
        if (idx < consumed_up_to_) {
            next_segment_to_download_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        std::vector<uint8_t> seg_data;
        if (!DownloadUrl(url.c_str(), seg_data)) {
            retry_count++;
            if (retry_count >= 3) {
                LOG_WARN("HLS: Skipping segment %d after %d failures", idx, 3);
                next_segment_to_download_.store(idx + 1, std::memory_order_release);
                retry_count = 0;
                continue;
            }
            LOG_WARN("HLS: Failed to download segment %d (attempt %d/3), retrying",
                     idx, retry_count);
            Sleep(1000);
            continue;
        }
        retry_count = 0;

        byte_stream_->AddSegment(seg_data.data(), seg_data.size());
        consumed_up_to_ = idx + 1;
        LOG_INFO("HLS: Segment %d: %zu bytes", idx + 1, seg_data.size());

        next_segment_to_download_.store(idx + 1, std::memory_order_release);
    }
}

void HlsManager::Close() {
    download_running_ = false;
    SetEvent(wake_event_);
    if (download_thread_) {
        // INFINITE wait: download loop checks download_running_ between operations.
        // WinHTTP built-in timeouts (up to ~30s worst case) bound the max wait.
        WaitForSingleObject(download_thread_, INFINITE);
        CloseHandle(download_thread_);
        download_thread_ = nullptr;
    }
    if (byte_stream_) {
        byte_stream_->Close();
        byte_stream_->Release();
        byte_stream_ = nullptr;
    }
    is_live_ = false;
    duration_ = 0;
}

HlsByteStream* HlsManager::GetByteStream() {
    return byte_stream_;
}

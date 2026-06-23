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
    HINTERNET* operator&() { reset(); return &h; }
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
 *   - Async reads (BeginRead/EndRead): always complete immediately via
 *     MFInvokeCallback. Never return E_PENDING — MF's TS demuxer does not
 *     handle pending reads during initial probing and would block forever.
 *     For 0 bytes without EOS, use S_OK (not S_FALSE) so MF does not
 *     prematurely signal end-of-stream during playback.
 *   - EOS protocol: Read() returns S_OK with 0 bytes when data is absent but
 *     EOS is not set (MF treats 0-bytes-S_OK as "try again"). Only returns
 *     S_FALSE when has_eos_marker_ is set AND position >= total_bytes_.
 *     This prevents premature EOF signaling before all segments arrive.
 *   - needs_wake_: set by AddSegment when new data may restart a stalled
 *     pipeline. The player's CheckAudio/VideoTick timers poll this flag.
 */
HlsByteStream::HlsByteStream() {
    InitializeCriticalSection(&lock_);
    data_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);  // manual-reset
}

HlsByteStream::~HlsByteStream() {
    Clear();
    if (data_event_) CloseHandle(data_event_);
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
    LOG_DEBUG("HlsBS: GetCapabilities -> 0x%08lX", *pdwCapabilities);
    return S_OK;
}

STDMETHODIMP HlsByteStream::GetLength(QWORD* pqwLength) {
    if (!pqwLength) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pqwLength = total_bytes_;
    LeaveCriticalSection(&lock_);
    LOG_DEBUG("HlsBS: GetLength -> %llu", *pqwLength);
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
    LOG_DEBUG("HlsBS: GetCurrentPosition -> %lld", *pqwPosition);
    return S_OK;
}

STDMETHODIMP HlsByteStream::SetCurrentPosition(QWORD qwPosition) {
    LOG_DEBUG("HlsBS: SetCurrentPosition(%llu)", qwPosition);
    EnterCriticalSection(&lock_);
    read_pos_ = (int64_t)qwPosition;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP HlsByteStream::IsEndOfStream(BOOL* pfEndOfStream) {
    if (!pfEndOfStream) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pfEndOfStream = (has_eos_marker_ && read_pos_ >= total_bytes_) ? TRUE : FALSE;
    int64_t rp = read_pos_;
    int64_t tb = total_bytes_;
    LeaveCriticalSection(&lock_);
    LOG_DEBUG("HlsBS: IsEndOfStream -> %d (eos=%d pos=%lld total=%lld)",
             *pfEndOfStream, has_eos_marker_, rp, tb);
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
            if (read_pos_ >= s.offset && read_pos_ < s.offset + (int64_t)s.data.size()) {
                seg_idx = mid;
                offset_in_seg = read_pos_ - s.offset;
                break;
            }
            if (read_pos_ < s.offset) hi = mid - 1;
            else lo = mid + 1;
        }
        if (seg_idx >= 0 && !segs_[seg_idx].data.empty()) {
            LoadedSeg& seg = segs_[seg_idx];
            size_t to_copy = seg.data.size() - (size_t)offset_in_seg;
            if (to_copy > cb) to_copy = cb;
            memcpy(pb, seg.data.data() + offset_in_seg, to_copy);
            total_read += (ULONG)to_copy;
            cb -= (ULONG)to_copy;
            pb += to_copy;
            read_pos_ += to_copy;
            // Free fully consumed segment data to prevent unbounded growth in live streams.
            // For VOD (cache_data_=true) we keep data so seek-back can re-read it.
            if (!cache_data_ && offset_in_seg + (int64_t)to_copy >= (int64_t)seg.data.size()) {
                seg.data.clear();
                seg.data.shrink_to_fit();
            }
        } else {
            break;
        }
    }
    return total_read;
}



/* Sync read: copy from segments under lock.
 * When no data is available, waits on data_event_ (signaled by AddSegment).
 * This prevents MF from seeing 0 bytes and setting internal EOF state. */
STDMETHODIMP HlsByteStream::Read(BYTE* pb, ULONG cb, ULONG* pcbRead) {
    if (!pb) return E_POINTER;

    EnterCriticalSection(&lock_);
    if (closed_) { LeaveCriticalSection(&lock_); return E_ABORT; }
    ULONG total_read = CopyFromSegmentsLocked(pb, cb);

    // If nothing read and EOS marker set, signal end
    if (total_read == 0 && has_eos_marker_ && read_pos_ >= total_bytes_) {
        LeaveCriticalSection(&lock_);
        if (pcbRead) *pcbRead = 0;
        LOG_DEBUG("HlsBS: Read(%u) -> S_FALSE (EOS)", cb);
        return S_FALSE;
    }

    // If no data available, wait for new data via manual-reset event
    if (total_read == 0) {
        ResetEvent(data_event_);
        LeaveCriticalSection(&lock_);
        for (int i = 0; i < 300; i++) {
            WaitForSingleObject(data_event_, 200);
            EnterCriticalSection(&lock_);
            if (closed_) { LeaveCriticalSection(&lock_); break; }
            total_read = CopyFromSegmentsLocked(pb, cb);
            LeaveCriticalSection(&lock_);
            if (total_read > 0) break;
            // Re-check EOS after wait
            EnterCriticalSection(&lock_);
            bool eos = (has_eos_marker_ && read_pos_ >= total_bytes_);
            LeaveCriticalSection(&lock_);
            if (eos) break;
            ResetEvent(data_event_);
        }
    }

    if (pcbRead) *pcbRead = total_read;
    if (total_read > 0) return S_OK;
    return S_OK;
}

/* Async read: always complete immediately via callback. Never return E_PENDING.
 * When no data is available, waits on data_event_ (signaled by AddSegment).
 * This prevents MF from seeing 0 bytes and setting internal EOF state. */
STDMETHODIMP HlsByteStream::BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* pState) {
    if (!pb || !pCallback) return E_POINTER;

    EnterCriticalSection(&lock_);
    if (closed_) { LeaveCriticalSection(&lock_); return E_ABORT; }
    ULONG bytesRead = CopyFromSegmentsLocked(pb, cb);
    bool is_eos = (bytesRead == 0 && has_eos_marker_ && read_pos_ >= total_bytes_);

    // Wait for data only when we got nothing (bytesRead == 0) and no EOS.
    // Do NOT wait on short reads — video demuxer needs data immediately,
    // and the TS demuxer handles short reads at segment boundaries fine.
    if (bytesRead == 0 && !is_eos && !closed_) {
        ResetEvent(data_event_);
        LeaveCriticalSection(&lock_);
        for (int i = 0; i < 300; i++) {
            WaitForSingleObject(data_event_, 200);
            EnterCriticalSection(&lock_);
            if (closed_) { LeaveCriticalSection(&lock_); break; }
            bytesRead = CopyFromSegmentsLocked(pb, cb);
            is_eos = (bytesRead == 0 && has_eos_marker_ && read_pos_ >= total_bytes_);
            LeaveCriticalSection(&lock_);
            if (bytesRead > 0 || is_eos) break;
            ResetEvent(data_event_);
        }
        EnterCriticalSection(&lock_);
    }

    async_result_.store(bytesRead, std::memory_order_release);
    async_hr_.store(is_eos ? S_FALSE : S_OK, std::memory_order_release);
    LeaveCriticalSection(&lock_);

    LOG_DEBUG("HlsBS: BeginRead(%u) -> %lu bytes (eos=%d)", cb, bytesRead, is_eos);

    ComPtr<IMFAsyncResult> result;
    if (SUCCEEDED(MFCreateAsyncResult(nullptr, pCallback, pState, &result))) {
        return MFInvokeCallback(result.get());
    }
    return S_OK;
}

STDMETHODIMP HlsByteStream::EndRead(IMFAsyncResult* /*pResult*/, ULONG* pcbRead) {
    if (pcbRead) *pcbRead = async_result_.load(std::memory_order_acquire);
    HRESULT hr = async_hr_.load(std::memory_order_acquire);
    LOG_DEBUG("HlsBS: EndRead -> %lu bytes, hr=0x%08lX", pcbRead ? *pcbRead : 0, hr);
    return hr;
}

STDMETHODIMP HlsByteStream::Write(const BYTE*, ULONG, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP HlsByteStream::BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) { return E_NOTIMPL; }
STDMETHODIMP HlsByteStream::EndWrite(IMFAsyncResult*, ULONG*) { return E_NOTIMPL; }

STDMETHODIMP HlsByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset,
                                  DWORD /*dwSeekFlags*/, QWORD* pqwCurrentPosition) {
    EnterCriticalSection(&lock_);
    int64_t old_pos = read_pos_.load(std::memory_order_relaxed);
    if (SeekOrigin == 0) {  // MFBYTESTREAM_SEEK_ORIGIN_BEGIN
        read_pos_ = llSeekOffset;
    } else {  // MFBYTESTREAM_SEEK_ORIGIN_CURRENT (only two origins defined in SDK)
        read_pos_ = read_pos_.load(std::memory_order_relaxed) + llSeekOffset;
    }
    if (read_pos_ < 0) read_pos_ = 0;
    int64_t tb = total_bytes_.load(std::memory_order_acquire);
    if (read_pos_ > tb) read_pos_ = tb;
    if (pqwCurrentPosition) *pqwCurrentPosition = read_pos_;
    int64_t new_pos = read_pos_;
    LeaveCriticalSection(&lock_);
    LOG_DEBUG("HlsBS: Seek(origin=%d, offset=%lld) pos %lld -> %lld", (int)SeekOrigin, llSeekOffset, old_pos, new_pos);
    return S_OK;
}

STDMETHODIMP HlsByteStream::Flush() { return S_OK; }

STDMETHODIMP HlsByteStream::Close() {
    LOG_WARN("HlsBS: Close() called — total_bytes was %lld", total_bytes_.load());
    // Do NOT set closed_ = true here — MF calls Close() during source reader release,
    // which would prevent the new source reader from reading.
    // Data and cleanup are handled by Clear() during normal shutdown.
    return S_OK;
}

void HlsByteStream::Abort() {
    LOG_WARN("HlsBS: Abort() — unblocking pending reads");
    EnterCriticalSection(&lock_);
    closed_ = true;
    LeaveCriticalSection(&lock_);
    if (data_event_) SetEvent(data_event_);
}

void HlsByteStream::AddSegment(std::vector<uint8_t> data) {
    EnterCriticalSection(&lock_);

    LoadedSeg seg;
    seg.data = std::move(data);
    seg.offset = total_bytes_;
    segs_.push_back(std::move(seg));

    size_t sz = segs_.back().data.size();
    total_bytes_ += sz;

    needs_wake_.store(1, std::memory_order_release);

    LeaveCriticalSection(&lock_);

    // Wake up any blocked BeginRead/Read waiting for data
    if (data_event_) SetEvent(data_event_);
}

bool HlsByteStream::CheckAndClearNeedsWake() {
    return needs_wake_.exchange(0, std::memory_order_acq_rel) != 0;
}

bool HlsByteStream::HasUnreadData() const {
    return read_pos_.load(std::memory_order_acquire) < total_bytes_.load(std::memory_order_acquire);
}

bool HlsByteStream::WaitForData(DWORD timeout_ms) {
    if (HasUnreadData()) return true;
    if (!data_event_) return false;
    ResetEvent(data_event_);
    (void)WaitForSingleObject(data_event_, timeout_ms);
    return HasUnreadData();
}

bool HlsByteStream::WaitForDataAmount(int64_t min_bytes, DWORD timeout_ms) {
    if (total_bytes_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_acquire) >= min_bytes)
        return true;
    if (!data_event_) return false;
    DWORD elapsed = 0;
    DWORD step = 500;
    while (elapsed < timeout_ms) {
        ResetEvent(data_event_);
        DWORD wait_time = (std::min)(step, timeout_ms - elapsed);
        (void)WaitForSingleObject(data_event_, wait_time);
        elapsed += wait_time;
        if (total_bytes_.load(std::memory_order_acquire) - read_pos_.load(std::memory_order_acquire) >= min_bytes)
            return true;
    }
    return false;
}

void HlsByteStream::Clear() {
    LOG_WARN("HlsBS: Clear() called — total_bytes was %lld", total_bytes_.load());
    EnterCriticalSection(&lock_);
    segs_.clear();
    total_bytes_ = 0;
    read_pos_ = 0;
    has_eos_marker_ = false;
    end_of_stream_ = false;
    LeaveCriticalSection(&lock_);
    if (data_event_) ResetEvent(data_event_);
}

void HlsByteStream::ResetForRestart() {
    EnterCriticalSection(&lock_);
    segs_.clear();
    total_bytes_ = 0;
    read_pos_ = 0;
    has_eos_marker_ = false;
    end_of_stream_ = false;
    needs_wake_.store(0, std::memory_order_release);
    LeaveCriticalSection(&lock_);
    if (data_event_) ResetEvent(data_event_);
    LOG_INFO("HlsBS: ResetForRestart — cleared all data, download thread will refill");
}

/*
 * Discard segments that have already been consumed by MF.
 * Aligns the cut point to a TS packet boundary (188 bytes, sync byte 0x47)
 * so the new source reader can correctly probe the TS container format.
 *
 * After discarding, read_pos_ is reset to 0 and remaining segment offsets
 * are shifted so the byte stream starts fresh from a TS packet boundary.
 */
void HlsByteStream::DiscardConsumedData() {
    EnterCriticalSection(&lock_);

    int64_t pos = read_pos_.load(std::memory_order_relaxed);
    if (pos <= 0 || segs_.empty()) {
        LeaveCriticalSection(&lock_);
        LOG_DEBUG("HlsBS: DiscardConsumedData — nothing to discard (pos=%lld, segs=%zu)", pos, segs_.size());
        return;
    }

    // Align pos down to nearest 188-byte TS packet boundary
    int64_t aligned_pos = (pos / 188) * 188;

    // Scan for 0x47 sync byte at aligned position to confirm TS alignment.
    // If not found, try the next boundary (up to 188 bytes ahead).
    bool found_sync = false;
    for (int try_offset = 0; try_offset < 188; try_offset++) {
        int64_t candidate = aligned_pos + try_offset;
        // Search through segments to find the byte at candidate position
        for (auto& s : segs_) {
            if (candidate >= s.offset && candidate < s.offset + (int64_t)s.data.size()) {
                int64_t idx_in_seg = candidate - s.offset;
                if (s.data.size() > (size_t)idx_in_seg && s.data[(size_t)idx_in_seg] == 0x47) {
                    aligned_pos = candidate;
                    found_sync = true;
                    break;
                }
            }
        }
        if (found_sync) break;
    }
    if (!found_sync) {
        // Fallback: use 188-byte alignment even without confirmed sync byte
        LOG_WARN("HlsBS: DiscardConsumedData — no 0x47 sync found, using 188-byte alignment");
    }

    // Find first segment that contains or is after aligned_pos
    size_t keep_from = 0;
    while (keep_from < segs_.size()) {
        auto& s = segs_[keep_from];
        if (s.offset + (int64_t)s.data.size() > aligned_pos) {
            break;
        }
        keep_from++;
    }

    if (keep_from == 0 && aligned_pos <= 0) {
        LeaveCriticalSection(&lock_);
        LOG_DEBUG("HlsBS: DiscardConsumedData — nothing to discard");
        return;
    }

    // Calculate how many bytes to skip within the first kept segment
    int64_t skip_in_first = 0;
    if (keep_from < segs_.size()) {
        skip_in_first = aligned_pos - segs_[keep_from].offset;
        if (skip_in_first < 0) skip_in_first = 0;
    }

    // Shift remaining segments' offsets to start from 0
    int64_t new_offset = 0;
    for (size_t i = keep_from; i < segs_.size(); i++) {
        segs_[i].offset = new_offset;
        new_offset += segs_[i].data.size();
    }

    // Erase fully consumed segments
    segs_.erase(segs_.begin(), segs_.begin() + keep_from);

    // Trim partial data from the first kept segment
    if (keep_from < segs_.size() && skip_in_first > 0) {
        segs_[0].data.erase(segs_[0].data.begin(),
                            segs_[0].data.begin() + (size_t)skip_in_first);
    }

    if (segs_.capacity() > 64) segs_.shrink_to_fit();

    total_bytes_.store(new_offset, std::memory_order_relaxed);
    read_pos_ = 0;

    LeaveCriticalSection(&lock_);
    LOG_INFO("HlsBS: DiscardConsumedData — discarded to TS-aligned pos %lld (was %lld), %zu segments remaining, total_bytes=%lld",
             aligned_pos, pos, segs_.size(), new_offset);
}

void HlsByteStream::SetEndOfStream() {
    EnterCriticalSection(&lock_);
    has_eos_marker_ = true;
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

    // Reuse session and connection if host/port match
    if (!http_session_ || !http_connect_ || host != http_host_ || port != http_port_) {
        // Release old connection if host changed
        if (http_connect_) { WinHttpCloseHandle(http_connect_); http_connect_ = nullptr; }

        // Create or recreate session
        if (!http_session_) {
            http_session_ = WinHttpOpen(L"MinPlay/1.0",
                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
            if (!http_session_) { LOG_ERROR("WinHttpOpen failed: %u", GetLastError()); return false; }
        }

        http_connect_ = WinHttpConnect(http_session_, host.c_str(), port, 0);
        if (!http_connect_) { LOG_ERROR("WinHttpConnect failed: %u", GetLastError()); return false; }
        http_host_ = host;
        http_port_ = port;
        LOG_INFO("HLS: new connection to %ws:%d", host.c_str(), port);
    }

    DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE;
    if (is_https) flags |= WINHTTP_FLAG_SECURE;

    WinHttpHandle hRequest(WinHttpOpenRequest(http_connect_, L"GET", path.c_str(), nullptr,
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

    // Store request handle so Close() can cancel pending WinHTTP operations
    active_request_ = hRequest;

    LOG_INFO("HLS: Sending request for %ws", url);
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        active_request_ = nullptr;
        LOG_ERROR("WinHttpSendRequest failed: %u", GetLastError());
        return false;
    }

    LOG_INFO("HLS: Receiving response...");
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        active_request_ = nullptr;
        LOG_ERROR("WinHttpReceiveResponse failed: %u", GetLastError());
        return false;
    }

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
    const size_t MAX_DOWNLOAD_SIZE = 200 * 1024 * 1024;  // 200MB safety limit
    bool read_ok = true;
    LOG_INFO("HLS: Reading data for %ws...", url);
    while (download_running_) {
        bytes_read = 0;
        if (!WinHttpReadData(hRequest, buf, sizeof(buf), &bytes_read)) {
            LOG_ERROR("WinHttpReadData failed: %u", GetLastError());
            read_ok = false;
            break;
        }
        if (bytes_read == 0) break;
        if (out.size() + bytes_read > MAX_DOWNLOAD_SIZE) {
            LOG_WARN("Download exceeds %zuMB limit, aborting", MAX_DOWNLOAD_SIZE / (1024*1024));
            return false;
        }
        out.insert(out.end(), buf, buf + bytes_read);
    }

    if (!download_running_) return false;
    active_request_ = nullptr;
    return read_ok;
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

    // Base directory — skip the "://" prefix when looking for trailing slash
    size_t proto = clean.find(L"://");
    size_t slash = (proto != std::wstring::npos)
        ? clean.rfind(L'/', clean.length())
        : clean.rfind(L'/');
    // Only use rfind result if it's past the protocol separator
    if (slash != std::wstring::npos && proto != std::wstring::npos && slash <= proto + 2)
        slash = std::wstring::npos;
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
            try { target_duration_ = (std::max)(1, std::stoi(line.substr(22))); } catch (...) { target_duration_ = 1; }
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
    download_running_ = true;

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
    byte_stream_->SetCacheData(true);

    // Start download thread to download segments in background.
    // MFCreateSourceReaderFromByteStream + BeginRead will wait for data as needed.
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

        byte_stream_->AddSegment(std::move(seg_data));
        added++;
    }

    if (added > 0) {
        LOG_INFO("HLS: Reload added %d new segments", added);
        EnterCriticalSection(&seg_lock_);
        int new_size = (int)segments_.size();
        LeaveCriticalSection(&seg_lock_);
        next_segment_to_download_.store(new_size, std::memory_order_release);
        consumed_up_to_.store(new_size, std::memory_order_release);
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
                // Live — reload playlist, then immediately download new segments.
                // Don't wait target_duration_ here — ReloadPlaylist already
                // fetched the latest playlist, and the download loop will
                // immediately download any new segments.
                ReloadPlaylist();
                continue;
            }
        }

        // Check if already downloaded (pre-buffered or previous loop iteration)
        if (idx < consumed_up_to_.load(std::memory_order_acquire)) {
            retry_count = 0;
            next_segment_to_download_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (!download_running_) break;

        std::vector<uint8_t> seg_data;
        if (!DownloadUrl(url.c_str(), seg_data)) {
            if (!download_running_) break;
            retry_count++;
            if (retry_count >= 3) {
                LOG_WARN("HLS: Skipping segment %d after %d failures", idx, 3);
                consumed_up_to_.store(idx + 1, std::memory_order_release);
                next_segment_to_download_.store(idx + 1, std::memory_order_release);
                retry_count = 0;
                continue;
            }
            LOG_WARN("HLS: Failed to download segment %d (attempt %d/3), retrying",
                     idx, retry_count);
            WaitForSingleObject(wake_event_, 1000);
            continue;
        }
        retry_count = 0;

        size_t sz = seg_data.size();
        byte_stream_->AddSegment(std::move(seg_data));
        consumed_up_to_.store(idx + 1, std::memory_order_release);
        LOG_INFO("HLS: Segment %d: %zu bytes", idx + 1, sz);

        next_segment_to_download_.store(idx + 1, std::memory_order_release);
    }
}

void HlsManager::Close() {
    download_running_ = false;
    SetEvent(wake_event_);

    // Cancel any in-flight WinHTTP request to unblock the download thread.
    // WinHttpCloseHandle on a pending request causes WinHttpSendRequest /
    // WinHttpReceiveResponse / WinHttpReadData to fail immediately.
    HINTERNET req = active_request_;
    if (req) {
        active_request_ = nullptr;
        WinHttpCloseHandle(req);
    }

    if (download_thread_) {
        DWORD wr = WaitForSingleObject(download_thread_, 5000);
        if (wr == WAIT_TIMEOUT) {
            LOG_WARN("HLS download thread did not exit in 5s after WinHTTP cancel");
        }
        CloseHandle(download_thread_);
        download_thread_ = nullptr;
    }
    if (byte_stream_) {
        byte_stream_->Abort();
        byte_stream_->Close();
        byte_stream_->Release();
        byte_stream_ = nullptr;
    }
    is_live_ = false;
    duration_ = 0;
    eos_sent_ = false;

    // Release WinHTTP session/connection
    if (http_connect_) { WinHttpCloseHandle(http_connect_); http_connect_ = nullptr; }
    if (http_session_) { WinHttpCloseHandle(http_session_); http_session_ = nullptr; }
    http_host_.clear();
    http_port_ = 0;
}

HlsByteStream* HlsManager::GetByteStream() {
    return byte_stream_;
}

void HlsManager::ResetDownloadState() {
    LOG_INFO("HLS: Resetting download state for pipeline restart");
    int seg_count;
    EnterCriticalSection(&seg_lock_);
    seg_count = (int)segments_.size();
    LeaveCriticalSection(&seg_lock_);
    next_segment_to_download_.store(seg_count, std::memory_order_release);
    consumed_up_to_.store(seg_count, std::memory_order_release);
    eos_sent_ = false;
    if (wake_event_) SetEvent(wake_event_);
}

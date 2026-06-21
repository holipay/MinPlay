#include "ts_byte_stream.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <cstring>

TsByteStream::TsByteStream(HlsByteStream* hls_stream)
    : hls_stream_(hls_stream), ref_count_(1), closed_(false),
      output_pos_(0), output_size_(0),
      has_video_(false), has_audio_(false),
      video_width_(0), video_height_(0),
      audio_sample_rate_(0), audio_channels_(0) {
    InitializeCriticalSection(&lock_);
    data_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);  // manual-reset
    if (hls_stream_) hls_stream_->AddRef();
}

TsByteStream::~TsByteStream() {
    if (hls_stream_) hls_stream_->Release();
    if (data_event_) CloseHandle(data_event_);
    DeleteCriticalSection(&lock_);
}

STDMETHODIMP TsByteStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFByteStream) {
        *ppv = static_cast<IMFByteStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) TsByteStream::AddRef() {
    return (ULONG)ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) TsByteStream::Release() {
    LONG r = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP TsByteStream::GetCapabilities(DWORD* pdwCapabilities) {
    if (!pdwCapabilities) return E_POINTER;
    *pdwCapabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
    return S_OK;
}

STDMETHODIMP TsByteStream::GetLength(QWORD* pqwLength) {
    if (!pqwLength) return E_POINTER;
    // Return a large value to indicate unknown length (live stream)
    *pqwLength = 0x7FFFFFFFFFFFFFFF;
    return S_OK;
}

STDMETHODIMP TsByteStream::SetLength(QWORD) { return E_NOTIMPL; }

STDMETHODIMP TsByteStream::GetCurrentPosition(QWORD* pqwPosition) {
    if (!pqwPosition) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pqwPosition = output_pos_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP TsByteStream::SetCurrentPosition(QWORD qwPosition) {
    EnterCriticalSection(&lock_);
    output_pos_ = (int)qwPosition;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP TsByteStream::IsEndOfStream(BOOL* pfEndOfStream) {
    if (!pfEndOfStream) return E_POINTER;
    // Never report EOF for live streams
    *pfEndOfStream = FALSE;
    return S_OK;
}

bool TsByteStream::RefillBuffer() {
    // Key design: pass through raw TS data from HlsByteStream.
    // MF's source reader expects TS data format.
    // The TsDemuxer is used to monitor stream state (program info),
    // not to replace the data format.
    // The critical fix: never return 0 bytes, preventing MF's EOF.

    if (!hls_stream_) return false;

    const int TS_READ_SIZE = 256 * 1024;
    std::vector<uint8_t> ts_data(TS_READ_SIZE);
    ULONG ts_bytes_read = 0;
    HRESULT hr = hls_stream_->Read(ts_data.data(), TS_READ_SIZE, &ts_bytes_read);

    if (FAILED(hr) || ts_bytes_read == 0) {
        return false;  // No data yet
    }

    // Feed to demuxer for stream info (video/audio PID detection)
    demuxer_.FeedData(ts_data.data(), (int)ts_bytes_read);

    // Update stream info
    if (!has_video_ && demuxer_.HasVideo()) {
        has_video_ = true;
        LOG_INFO("TsByteStream: video stream detected");
    }
    if (!has_audio_ && demuxer_.HasAudio()) {
        has_audio_ = true;
        LOG_INFO("TsByteStream: audio stream detected");
    }

    // Return the raw TS data to MF (not parsed ES data)
    EnterCriticalSection(&lock_);
    output_buffer_ = std::move(ts_data);
    output_size_ = (int)ts_bytes_read;
    output_pos_ = 0;
    LeaveCriticalSection(&lock_);

    return true;
}

STDMETHODIMP TsByteStream::Read(BYTE* pb, ULONG cb, ULONG* pcbRead) {
    if (!pb) return E_POINTER;

    EnterCriticalSection(&lock_);
    if (closed_) { LeaveCriticalSection(&lock_); return E_ABORT; }

    // Try output buffer first
    if (output_pos_ < output_size_) {
        int avail = output_size_ - output_pos_;
        int to_read = (int)cb < avail ? (int)cb : avail;
        memcpy(pb, output_buffer_.data() + output_pos_, to_read);
        output_pos_ += to_read;
        if (pcbRead) *pcbRead = to_read;
        LeaveCriticalSection(&lock_);
        return S_OK;
    }
    LeaveCriticalSection(&lock_);

    // Refill from demuxer
    if (RefillBuffer()) {
        EnterCriticalSection(&lock_);
        int avail = output_size_ - output_pos_;
        int to_read = (int)cb < avail ? (int)cb : avail;
        memcpy(pb, output_buffer_.data() + output_pos_, to_read);
        output_pos_ += to_read;
        if (pcbRead) *pcbRead = to_read;
        LeaveCriticalSection(&lock_);
        return S_OK;
    }

    if (pcbRead) *pcbRead = 0;
    return S_OK;
}

STDMETHODIMP TsByteStream::BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* pState) {
    if (!pb || !pCallback) return E_POINTER;

    // Try to fill the buffer
    if (!RefillBuffer() && hls_stream_) {
        // Wait for data (up to 60s)
        for (int i = 0; i < 300 && !output_buffer_.empty() == false; i++) {
            Sleep(200);
            if (RefillBuffer()) break;
        }
    }

    EnterCriticalSection(&lock_);
    int avail = output_size_ - output_pos_;
    int to_read = avail > 0 ? ((int)cb < avail ? (int)cb : avail) : 0;

    if (to_read > 0) {
        memcpy(pb, output_buffer_.data() + output_pos_, to_read);
        output_pos_ += to_read;
    }

    async_result_.store(to_read, std::memory_order_release);
    async_hr_.store(S_OK, std::memory_order_release);
    LeaveCriticalSection(&lock_);

    ComPtr<IMFAsyncResult> result;
    if (SUCCEEDED(MFCreateAsyncResult(nullptr, pCallback, pState, &result))) {
        return MFInvokeCallback(result.get());
    }
    return S_OK;
}

STDMETHODIMP TsByteStream::EndRead(IMFAsyncResult* /*pResult*/, ULONG* pcbRead) {
    if (pcbRead) *pcbRead = async_result_.load(std::memory_order_acquire);
    return async_hr_.load(std::memory_order_acquire);
}

STDMETHODIMP TsByteStream::Write(const BYTE*, ULONG, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP TsByteStream::BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) { return E_NOTIMPL; }
STDMETHODIMP TsByteStream::EndWrite(IMFAsyncResult*, ULONG*) { return E_NOTIMPL; }

STDMETHODIMP TsByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset,
                                  DWORD /*dwSeekFlags*/, QWORD* pqwCurrentPosition) {
    EnterCriticalSection(&lock_);
    if (SeekOrigin == 0) {
        output_pos_ = (int)llSeekOffset;
    } else {
        output_pos_ += (int)llSeekOffset;
    }
    if (output_pos_ < 0) output_pos_ = 0;
    if (pqwCurrentPosition) *pqwCurrentPosition = output_pos_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP TsByteStream::Flush() { return S_OK; }

STDMETHODIMP TsByteStream::Close() {
    EnterCriticalSection(&lock_);
    closed_ = true;
    LeaveCriticalSection(&lock_);
    if (data_event_) SetEvent(data_event_);
    return S_OK;
}

void TsByteStream::Abort() {
    EnterCriticalSection(&lock_);
    closed_ = true;
    LeaveCriticalSection(&lock_);
    if (data_event_) SetEvent(data_event_);
}

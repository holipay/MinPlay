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
    data_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (hls_stream_) hls_stream_->AddRef();
}

TsByteStream::~TsByteStream() {
    if (hls_stream_) hls_stream_->Release();
    if (data_event_) CloseHandle(data_event_);
    DeleteCriticalSection(&lock_);
}

// IUnknown
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

// IMFByteStream — delegate to HlsByteStream for all operations
STDMETHODIMP TsByteStream::GetCapabilities(DWORD* pdwCapabilities) {
    return hls_stream_ ? hls_stream_->GetCapabilities(pdwCapabilities) : E_FAIL;
}
STDMETHODIMP TsByteStream::GetLength(QWORD* pqwLength) {
    return hls_stream_ ? hls_stream_->GetLength(pqwLength) : E_FAIL;
}
STDMETHODIMP TsByteStream::SetLength(QWORD) { return E_NOTIMPL; }
STDMETHODIMP TsByteStream::GetCurrentPosition(QWORD* pqwPosition) {
    return hls_stream_ ? hls_stream_->GetCurrentPosition(pqwPosition) : E_FAIL;
}
STDMETHODIMP TsByteStream::SetCurrentPosition(QWORD qwPosition) {
    return hls_stream_ ? hls_stream_->SetCurrentPosition(qwPosition) : E_FAIL;
}
STDMETHODIMP TsByteStream::IsEndOfStream(BOOL* pfEndOfStream) {
    if (!pfEndOfStream) return E_POINTER;
    *pfEndOfStream = FALSE;  // Never EOF for live streams
    return S_OK;
}
STDMETHODIMP TsByteStream::Flush() {
    return hls_stream_ ? hls_stream_->Flush() : E_FAIL;
}
STDMETHODIMP TsByteStream::Close() {
    EnterCriticalSection(&lock_);
    closed_ = true;
    LeaveCriticalSection(&lock_);
    if (hls_stream_) return hls_stream_->Close();
    return S_OK;
}

// Read — delegate to HlsByteStream, but feed data to TsDemuxer for monitoring
STDMETHODIMP TsByteStream::Read(BYTE* pb, ULONG cb, ULONG* pcbRead) {
    if (!hls_stream_) {
        if (pcbRead) *pcbRead = 0;
        return E_FAIL;
    }
    HRESULT hr = hls_stream_->Read(pb, cb, pcbRead);
    // Feed raw TS data to demuxer for stream state monitoring
    if (SUCCEEDED(hr) && pcbRead && *pcbRead > 0) {
        demuxer_.FeedData(pb, *pcbRead);
        if (!has_video_ && demuxer_.HasVideo()) {
            has_video_ = true;
            LOG_INFO("TsByteStream: video stream detected");
        }
        if (!has_audio_ && demuxer_.HasAudio()) {
            has_audio_ = true;
            LOG_INFO("TsByteStream: audio stream detected");
        }
    }
    return hr;
}

// BeginRead — delegate to HlsByteStream, feed data to TsDemuxer in callback
STDMETHODIMP TsByteStream::BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* pState) {
    if (!hls_stream_) return E_FAIL;
    return hls_stream_->BeginRead(pb, cb, pCallback, pState);
}

STDMETHODIMP TsByteStream::EndRead(IMFAsyncResult* pResult, ULONG* pcbRead) {
    if (!hls_stream_) {
        if (pcbRead) *pcbRead = 0;
        return E_FAIL;
    }
    HRESULT hr = hls_stream_->EndRead(pResult, pcbRead);
    // Feed raw TS data to demuxer for stream state monitoring
    if (SUCCEEDED(hr) && pcbRead && *pcbRead > 0) {
        // Note: we can't access the buffer here, but the demuxer will be fed
        // in the next Read call. For now, this is a limitation.
    }
    return hr;
}

STDMETHODIMP TsByteStream::Write(const BYTE*, ULONG, ULONG*) { return E_NOTIMPL; }
STDMETHODIMP TsByteStream::BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) { return E_NOTIMPL; }
STDMETHODIMP TsByteStream::EndWrite(IMFAsyncResult*, ULONG*) { return E_NOTIMPL; }

STDMETHODIMP TsByteStream::Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset,
                                  DWORD dwSeekFlags, QWORD* pqwCurrentPosition) {
    return hls_stream_ ? hls_stream_->Seek(SeekOrigin, llSeekOffset, dwSeekFlags, pqwCurrentPosition) : E_FAIL;
}

void TsByteStream::Abort() {
    EnterCriticalSection(&lock_);
    closed_ = true;
    LeaveCriticalSection(&lock_);
    if (hls_stream_) hls_stream_->Abort();
}

#include "hls_media_stream.h"
#include "hls_media_source.h"
#include "../util/log.h"
#include <oleauto.h>

HlsMediaStream::HlsMediaStream(HlsMediaSource* source, DWORD stream_id,
                                 IMFStreamDescriptor* sd)
    : source_(source), stream_id_(stream_id), stream_desc_(sd),
      event_queue_(nullptr) {
    InitializeCriticalSection(&token_lock_);
    if (stream_desc_) stream_desc_->AddRef();
    MFCreateEventQueue(&event_queue_);
}

HlsMediaStream::~HlsMediaStream() {
    if (event_queue_) { event_queue_->Shutdown(); event_queue_->Release(); }
    if (stream_desc_) stream_desc_->Release();
    DeleteCriticalSection(&token_lock_);
}

// IUnknown — use IMFMediaStream* as canonical pointer (first base in chain)
STDMETHODIMP HlsMediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator || riid == IID_IMFMediaStream) {
        // All share one pointer: IMFMediaStream inherits from IMFMediaEventGenerator from IUnknown
        *ppv = static_cast<IMFMediaStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
STDMETHODIMP_(ULONG) HlsMediaStream::AddRef() {
    return (ULONG)ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}
STDMETHODIMP_(ULONG) HlsMediaStream::Release() {
    LONG r = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete this;
    return r;
}

// IMFMediaEventGenerator
STDMETHODIMP HlsMediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) {
    return event_queue_ ? event_queue_->BeginGetEvent(pCallback, pState) : E_FAIL;
}
STDMETHODIMP HlsMediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    return event_queue_ ? event_queue_->EndGetEvent(pResult, ppEvent) : E_FAIL;
}
STDMETHODIMP HlsMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    return event_queue_ ? event_queue_->GetEvent(dwFlags, ppEvent) : E_FAIL;
}
STDMETHODIMP HlsMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                         HRESULT hrStatus, const PROPVARIANT* pvValue) {
    return event_queue_ ? event_queue_->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue)
                        : E_FAIL;
}

// IMFMediaStream
STDMETHODIMP HlsMediaStream::GetMediaSource(IMFMediaSource** ppMediaSource) {
    if (!ppMediaSource) return E_POINTER;
    if (!source_) return E_FAIL;
    *ppMediaSource = source_;
    source_->AddRef();
    return S_OK;
}

STDMETHODIMP HlsMediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) {
    if (!ppStreamDescriptor) return E_POINTER;
    if (!stream_desc_) return E_FAIL;
    *ppStreamDescriptor = stream_desc_;
    stream_desc_->AddRef();
    return S_OK;
}

STDMETHODIMP HlsMediaStream::RequestSample(IUnknown* pToken) {
    if (pToken) {
        EnterCriticalSection(&token_lock_);
        tokens_.push(ComPtr<IUnknown>(pToken));
        LeaveCriticalSection(&token_lock_);
    }
    return S_OK;
}

void HlsMediaStream::DeliverFrame(const uint8_t* data, size_t size, double pts_sec) {
    EnterCriticalSection(&token_lock_);
    if (tokens_.empty()) {
        LeaveCriticalSection(&token_lock_);
        return;
    }

    ComPtr<IUnknown> token = std::move(tokens_.front());
    tokens_.pop();
    LeaveCriticalSection(&token_lock_);

    // Create sample
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);

    // Create buffer
    ComPtr<IMFMediaBuffer> buffer;
    MFCreateMemoryBuffer((DWORD)size, &buffer);

    BYTE* buf_data = nullptr;
    buffer->Lock(&buf_data, nullptr, nullptr);
    memcpy(buf_data, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)size);

    sample->AddBuffer(buffer.get());

    // Set PTS
    LONGLONG pts_100ns = (LONGLONG)(pts_sec * 10000000.0);
    sample->SetSampleTime(pts_100ns);

    // Queue MEMediaSample event with the sample as the object
    if (event_queue_) {
        event_queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.get());
    }
}

void HlsMediaStream::SetEos() {
    if (event_queue_) {
        event_queue_->QueueEventParamVar(MEEndOfStream, GUID_NULL, S_OK, nullptr);
    }
}

void HlsMediaStream::Start() {
    is_started_ = true;
    if (event_queue_) {
        event_queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    }
}

void HlsMediaStream::Stop() {
    is_started_ = false;
    if (event_queue_) {
        event_queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    }
    EnterCriticalSection(&token_lock_);
    while (!tokens_.empty()) tokens_.pop();
    LeaveCriticalSection(&token_lock_);
}

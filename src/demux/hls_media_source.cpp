#include "hls_media_source.h"
#include "../network/hls_stream.h"
#include "../util/log.h"

HlsMediaSource::HlsMediaSource(HlsByteStream* byte_stream)
    : byte_stream_(byte_stream), ref_count_(1),
      is_started_(false), is_shutdown_(false) {
    InitializeCriticalSection(&lock_);
    if (byte_stream_) byte_stream_->AddRef();
}

HlsMediaSource::~HlsMediaSource() {
    Shutdown();
    if (byte_stream_) byte_stream_->Release();
    DeleteCriticalSection(&lock_);
}

HRESULT HlsMediaSource::CreateInstance(HlsByteStream* byte_stream, IMFMediaSource** ppSource) {
    if (!ppSource) return E_POINTER;
    if (!byte_stream) return E_INVALIDARG;

    HlsMediaSource* source = new (std::nothrow) HlsMediaSource(byte_stream);
    if (!source) return E_OUTOFMEMORY;

    *ppSource = source;
    (*ppSource)->AddRef();
    return S_OK;
}

// IUnknown
STDMETHODIMP HlsMediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFMediaSource) {
        *ppv = static_cast<IMFMediaSource*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) HlsMediaSource::AddRef() {
    return ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) HlsMediaSource::Release() {
    LONG r = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete this;
    return r;
}

// IMFMediaSource
STDMETHODIMP HlsMediaSource::GetCharacteristics(DWORD* pdwCharacteristics) {
    if (!pdwCharacteristics) return E_POINTER;
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP HlsMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD) {
    if (!ppPD) return E_POINTER;
    // TODO: Create presentation descriptor with video/audio streams
    return E_NOTIMPL;
}

STDMETHODIMP HlsMediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                                     const PROPVARIANT* pvarStartPosition) {
    if (is_started_) return E_FAIL;
    is_started_ = true;
    return S_OK;
}

STDMETHODIMP HlsMediaSource::Stop() {
    return S_OK;
}

STDMETHODIMP HlsMediaSource::Pause() {
    return E_NOTIMPL;
}

STDMETHODIMP HlsMediaSource::Shutdown() {
    if (is_shutdown_) return S_OK;
    is_shutdown_ = true;
    return S_OK;
}

// IMFMediaEventGenerator
STDMETHODIMP HlsMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) {
    return E_NOTIMPL;
}

STDMETHODIMP HlsMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    return E_NOTIMPL;
}

STDMETHODIMP HlsMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    return E_NOTIMPL;
}

STDMETHODIMP HlsMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                         HRESULT hrStatus, const PROPVARIANT* pvValue) {
    return S_OK;
}

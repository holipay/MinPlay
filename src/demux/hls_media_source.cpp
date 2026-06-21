#include "hls_media_source.h"
#include "../network/hls_stream.h"
#include "../util/log.h"

// MEReadSample event type - MediaEventType value for signaling data availability
#ifndef MEReadSample
#define MEReadSample ((MediaEventType)12)
#endif

HlsMediaSource::HlsMediaSource(HlsByteStream* byte_stream)
    : byte_stream_(byte_stream), ref_count_(1),
      is_started_(false), is_shutdown_(false),
      event_queue_(nullptr), read_thread_(nullptr), read_running_(false) {
    InitializeCriticalSection(&lock_);
    if (byte_stream_) byte_stream_->AddRef();
    MFCreateEventQueue(&event_queue_);
}

HlsMediaSource::~HlsMediaSource() {
    Shutdown();
    if (byte_stream_) byte_stream_->Release();
    if (event_queue_) { event_queue_->Shutdown(); event_queue_->Release(); }
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
    *ppPD = nullptr;

    // Create video media type
    ComPtr<IMFMediaType> vmt;
    HRESULT hr = MFCreateMediaType(&vmt);
    if (FAILED(hr)) return hr;
    vmt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vmt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(vmt.get(), MF_MT_FRAME_SIZE, 1280, 720);
    MFSetAttributeRatio(vmt.get(), MF_MT_FRAME_RATE, 30, 1);
    vmt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    vmt->SetUINT32(MF_MT_AVG_BITRATE, 2500000);

    // Create video stream descriptor
    ComPtr<IMFStreamDescriptor> video_sd;
    IMFMediaType* raw_mt = vmt.get();
    hr = MFCreateStreamDescriptor(0, 1, &raw_mt, &video_sd);
    if (FAILED(hr)) return hr;

    // Create presentation descriptor with video stream
    IMFStreamDescriptor* descs[1] = { video_sd.get() };
    hr = MFCreatePresentationDescriptor(1, descs, ppPD);
    if (FAILED(hr)) return hr;

    return S_OK;
}

STDMETHODIMP HlsMediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                                     const PROPVARIANT* pvarStartPosition) {
    if (is_started_) return E_FAIL;
    is_started_ = true;
    read_running_ = true;
    read_thread_ = CreateThread(nullptr, 0, ReadThreadProc, this, 0, nullptr);
    return S_OK;
}

STDMETHODIMP HlsMediaSource::Stop() {
    read_running_ = false;
    if (read_thread_) {
        WaitForSingleObject(read_thread_, 5000);
        CloseHandle(read_thread_);
        read_thread_ = nullptr;
    }
    return S_OK;
}

STDMETHODIMP HlsMediaSource::Pause() {
    return E_NOTIMPL;
}

STDMETHODIMP HlsMediaSource::Shutdown() {
    if (is_shutdown_) return S_OK;
    is_shutdown_ = true;
    Stop();
    if (event_queue_) event_queue_->Shutdown();
    return S_OK;
}

// IMFMediaEventGenerator
STDMETHODIMP HlsMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) {
    if (!event_queue_) return E_FAIL;
    return event_queue_->BeginGetEvent(pCallback, pState);
}

STDMETHODIMP HlsMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (!event_queue_) return E_FAIL;
    return event_queue_->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP HlsMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (!event_queue_) return E_FAIL;
    return event_queue_->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP HlsMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                         HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (!event_queue_) return E_FAIL;
    return event_queue_->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// Read thread
DWORD WINAPI HlsMediaSource::ReadThreadProc(LPVOID param) {
    ((HlsMediaSource*)param)->ReadLoop();
    return 0;
}

void HlsMediaSource::ReadLoop() {
    while (read_running_) {
        DemuxFrame frame;
        if (demuxer_.ReadAndDemux(byte_stream_)) {
            while (demuxer_.GetNextFrame(frame)) {
                // Queue event to notify MF that data is available
                if (event_queue_) {
                    event_queue_->QueueEventParamVar(MEReadSample, GUID_NULL, S_OK, nullptr);
                }
            }
        }
    }
}

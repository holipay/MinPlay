#include "hls_media_source.h"
#include "hls_media_stream.h"
#include "../network/hls_stream.h"
#include "../util/log.h"

// MEStarted not defined in SDK 10.0.28000 — value is 1 per MF spec
#ifndef MEStarted
#define MEStarted ((MediaEventType)1)
#endif

HlsMediaSource::HlsMediaSource(HlsByteStream* byte_stream)
    : byte_stream_(byte_stream), ref_count_(1),
      is_started_(false), is_shutdown_(false),
      event_queue_(nullptr), read_thread_(nullptr), read_running_(false),
      video_stream_(nullptr), audio_stream_(nullptr), source_attrs_(nullptr) {
    InitializeCriticalSection(&lock_);
    if (byte_stream_) byte_stream_->AddRef();
    MFCreateEventQueue(&event_queue_);
    MFCreateAttributes(&source_attrs_, 1);
}

HlsMediaSource::~HlsMediaSource() {
    Shutdown();
    if (video_stream_) { video_stream_->Release(); video_stream_ = nullptr; }
    if (audio_stream_) { audio_stream_->Release(); audio_stream_ = nullptr; }
    if (source_attrs_) { source_attrs_->Release(); source_attrs_ = nullptr; }
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
    if (riid == IID_IMFMediaSourceEx) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IMFMediaEventGenerator) {
        *ppv = static_cast<IMFMediaEventGenerator*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IMFGetService) {
        *ppv = static_cast<IMFGetService*>(this);
        AddRef();
        return S_OK;
    }
    // IMFAttributes — SourceReader queries this on the source
    if (riid == IID_IMFAttributes && source_attrs_) {
        *ppv = source_attrs_;
        source_attrs_->AddRef();
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
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE | MFMEDIASOURCE_CAN_PAUSE;
    return S_OK;
}

STDMETHODIMP HlsMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD) {
    if (!ppPD) return E_POINTER;
    *ppPD = nullptr;

    // Try to detect streams from byte data, but don't block if no data yet.
    // If no data available, assume H.264+AAC (most common for HLS).
    if (!demuxer_.HasVideo() && !demuxer_.HasAudio() && byte_stream_) {
        const int SCAN_SIZE = 256 * 1024;
        std::vector<uint8_t> scan_buf(SCAN_SIZE);
        ULONG bytes_read = 0;
        byte_stream_->Seek(MFBYTESTREAM_SEEK_ORIGIN(0), 0, 0, nullptr);
        HRESULT hr = byte_stream_->Read(scan_buf.data(), SCAN_SIZE, &bytes_read);
        if (SUCCEEDED(hr) && bytes_read > 0) {
            demuxer_.FeedData(scan_buf.data(), (int)bytes_read);
        }
        byte_stream_->Seek(MFBYTESTREAM_SEEK_ORIGIN(0), 0, 0, nullptr);
        if (!demuxer_.HasVideo() && !demuxer_.HasAudio()) {
            LOG_INFO("HlsMediaSource: no stream data detected, assuming H.264+AAC defaults");
        }
    }

    std::vector<IMFStreamDescriptor*> descs;
    ComPtr<IMFMediaType> vmt, amt;

    // Create video stream descriptor
    // Use default H.264 format — MF source reader will negotiate actual resolution
    {
        HRESULT hr = MFCreateMediaType(&vmt);
        if (FAILED(hr)) return hr;
        vmt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vmt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        // Don't set specific resolution — let MF negotiate from actual data
        vmt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vmt->SetUINT32(MF_MT_AVG_BITRATE, 5000000);

        ComPtr<IMFStreamDescriptor> video_sd;
        IMFMediaType* raw_mt = vmt.get();
        IMFMediaType* mt_arr[1] = { raw_mt };
        HRESULT hr2 = MFCreateStreamDescriptor(0, 1, mt_arr, video_sd.GetAddress());
        if (SUCCEEDED(hr2)) descs.push_back(video_sd.Detach());
    }

    // Create audio stream descriptor
    {
        HRESULT hr = MFCreateMediaType(&amt);
        if (FAILED(hr)) return hr;
        amt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        amt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        amt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        amt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        amt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        amt->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192000 / 8);

        ComPtr<IMFStreamDescriptor> audio_sd;
        IMFMediaType* raw_mt = amt.get();
        IMFMediaType* mt_arr[1] = { raw_mt };
        HRESULT hr2 = MFCreateStreamDescriptor(1, 1, mt_arr, audio_sd.GetAddress());
        if (SUCCEEDED(hr2)) descs.push_back(audio_sd.Detach());
    }

    if (descs.empty()) return E_FAIL;

    // Create stream objects for each descriptor
    for (DWORD i = 0; i < (DWORD)descs.size(); i++) {
        IMFStreamDescriptor* sd = descs[i];
        DWORD id = 0;
        sd->GetStreamIdentifier(&id);

        HlsMediaStream* stream = new (std::nothrow) HlsMediaStream(this, id, sd);
        if (!stream) {
            for (auto* s : descs) s->Release();
            return E_OUTOFMEMORY;
        }

        if (id == 0 && !video_stream_) {
            video_stream_ = stream;
            LOG_INFO("HlsMediaSource: created video stream (id=%lu)", id);
        } else if (id == 1 && !audio_stream_) {
            audio_stream_ = stream;
            LOG_INFO("HlsMediaSource: created audio stream (id=%lu)", id);
        }
    }

    HRESULT hr = MFCreatePresentationDescriptor((DWORD)descs.size(), descs.data(), ppPD);
    for (auto* sd : descs) sd->Release();
    return hr;
}

STDMETHODIMP HlsMediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                                     const PROPVARIANT* pvarStartPosition) {
    if (is_started_) return E_FAIL;
    is_started_ = true;

    // MF requires MEStarted on source event queue FIRST
    if (event_queue_) {
        event_queue_->QueueEventParamVar(MEStarted, GUID_NULL, S_OK, nullptr);
    }

    // Then MEStreamStarted on each stream's event queue
    if (video_stream_) video_stream_->QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);
    if (audio_stream_) audio_stream_->QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);

    // Start read thread
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

// IMFMediaSourceEx
STDMETHODIMP HlsMediaSource::GetSourceAttributes(IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    if (!source_attrs_) return E_FAIL;
    *ppAttributes = source_attrs_;
    source_attrs_->AddRef();
    return S_OK;
}

STDMETHODIMP HlsMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    *ppAttributes = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP HlsMediaSource::SetD3DManager(IUnknown* pManager) {
    return S_OK;
}

// IMFGetService
STDMETHODIMP HlsMediaSource::GetService(REFGUID guidService, REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

// Read thread
DWORD WINAPI HlsMediaSource::ReadThreadProc(LPVOID param) {
    ((HlsMediaSource*)param)->ReadLoop();
    return 0;
}

void HlsMediaSource::ReadLoop() {
    while (read_running_) {
        // Wait for streams to have pending requests before delivering frames.
        // The source reader calls RequestSample after Start(), so we need to
        // wait for this before delivering any data.
        bool video_ready = !video_stream_ || video_stream_->HasPendingRequest();
        bool audio_ready = !audio_stream_ || audio_stream_->HasPendingRequest();
        if (!video_ready && !audio_ready) {
            Sleep(5);
            continue;
        }

        DemuxFrame frame;
        if (demuxer_.ReadAndDemux(byte_stream_)) {
            while (demuxer_.GetNextFrame(frame)) {
                if (frame.type == DemuxFrame::Video && video_stream_ && video_stream_->HasPendingRequest()) {
                    video_stream_->DeliverFrame(frame.data.data(), frame.data.size(), frame.pts);
                } else if (frame.type == DemuxFrame::Audio && audio_stream_ && audio_stream_->HasPendingRequest()) {
                    audio_stream_->DeliverFrame(frame.data.data(), frame.data.size(), frame.pts);
                }
            }
        }

        // Yield briefly to avoid spinning when no data available
        if (!demuxer_.HasFrames()) {
            Sleep(5);
        }
    }

    // Signal end-of-stream on both streams
    if (video_stream_) video_stream_->SetEos();
    if (audio_stream_) audio_stream_->SetEos();
}

#include "hls_media_source.h"
#include "hls_media_stream.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <mfobjects.h>

// MEStarted not defined in SDK 10.0.28000 — value is 1 per MF spec
#ifndef MEStarted
#define MEStarted ((MediaEventType)1)
#endif
// MESourceInitialized not in this SDK version — value is 12 per MF spec
#ifndef MESourceInitialized
#define MESourceInitialized ((MediaEventType)12)
#endif
// MEStreamCreated not in this SDK version — value is 10 per MF spec
#ifndef MEStreamCreated
#define MEStreamCreated ((MediaEventType)10)
#endif
// MEStopped not in this SDK — use MESourceStopped (207) per MF spec
#ifndef MEStopped
#define MEStopped MESourceStopped
#endif

HlsMediaSource::HlsMediaSource(HlsByteStream* byte_stream, bool is_live)
    : byte_stream_(byte_stream), is_live_(is_live), ref_count_(1),
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

HRESULT HlsMediaSource::CreateInstance(HlsByteStream* byte_stream, bool is_live, IMFMediaSource** ppSource) {
    if (!ppSource) return E_POINTER;
    if (!byte_stream) return E_INVALIDARG;

    HlsMediaSource* source = new (std::nothrow) HlsMediaSource(byte_stream, is_live);
    if (!source) return E_OUTOFMEMORY;

    *ppSource = source;
    (*ppSource)->AddRef();
    return S_OK;
}

// IUnknown — COM identity rule: IID_IUnknown must return the same pointer as all QI-able interfaces.
// Use IMFMediaSourceEx* as the canonical pointer (first base in the chain).
STDMETHODIMP HlsMediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;

    // All interfaces on the primary chain share one pointer to satisfy COM identity
    if (riid == IID_IUnknown || riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) {
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
    DWORD flags = MFMEDIASOURCE_CAN_PAUSE;
    if (is_live_) flags |= MFMEDIASOURCE_IS_LIVE;
    *pdwCharacteristics = flags;
    return S_OK;
}

STDMETHODIMP HlsMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD) {
    if (!ppPD) return E_POINTER;
    *ppPD = nullptr;

    // Try to detect streams from byte data, but don't block if no data yet.
    if (!demuxer_.HasVideo() && !demuxer_.HasAudio() && byte_stream_) {
        const int SCAN_SIZE = 256 * 1024;
        std::vector<uint8_t> scan_buf(SCAN_SIZE);
        ULONG bytes_read = 0;
        byte_stream_->Seek(MFBYTESTREAM_SEEK_ORIGIN(0), 0, 0, nullptr);
        HRESULT hr = byte_stream_->Read(scan_buf.data(), SCAN_SIZE, &bytes_read);
        if (SUCCEEDED(hr) && bytes_read > 0) {
            demuxer_.FeedData(scan_buf.data(), (int)bytes_read);
            // Process frames to extract codec config (SPS/PPS, AudioSpecificConfig)
            DemuxFrame tmp;
            while (demuxer_.GetNextFrame(tmp)) {}
        }
        byte_stream_->Seek(MFBYTESTREAM_SEEK_ORIGIN(0), 0, 0, nullptr);
        if (!demuxer_.HasVideo() && !demuxer_.HasAudio()) {
            LOG_INFO("HlsMediaSource: no stream data detected, assuming H.264+AAC defaults");
        }
    }

    std::vector<IMFStreamDescriptor*> descs;

    // Create video stream descriptor — offer H.264 native format
    {
        ComPtr<IMFMediaType> vmt;
        HRESULT hr = MFCreateMediaType(&vmt);
        if (FAILED(hr)) return hr;
        vmt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vmt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        vmt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        vmt->SetUINT32(MF_MT_AVG_BITRATE, 5000000);
        MFSetAttributeRatio(vmt.get(), MF_MT_FRAME_RATE, 30, 1);
        vmt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        vmt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_10);
        vmt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);

        // Set H.264 codec config (SPS+PPS in avcC format) if extracted from TS data
        std::vector<uint8_t> video_config;
        if (demuxer_.GetVideoCodecConfig(video_config) && !video_config.empty()) {
            vmt->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, video_config.data(), (UINT32)video_config.size());
            LOG_INFO("HlsMediaSource: set video MF_MT_MPEG_SEQUENCE_HEADER (%zu bytes)", video_config.size());
        }

        // Set resolution from SPS — MF decoder MFT needs this to initialize
        int w = demuxer_.GetVideoWidth();
        int h = demuxer_.GetVideoHeight();
        if (w > 0 && h > 0) {
            MFSetAttributeSize(vmt.get(), MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
            vmt->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)w);
            MFSetAttributeRatio(vmt.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            LOG_INFO("HlsMediaSource: set MF_MT_FRAME_SIZE %dx%d", w, h);
        }

        ComPtr<IMFStreamDescriptor> video_sd;
        IMFMediaType* raw_mt = vmt.get();
        IMFMediaType* mt_arr[1] = { raw_mt };
        HRESULT hr2 = MFCreateStreamDescriptor(0, 1, mt_arr, video_sd.GetAddress());
        if (SUCCEEDED(hr2)) descs.push_back(video_sd.Detach());
    }

    // Create audio stream descriptor — offer AAC format
    {
        ComPtr<IMFMediaType> amt;
        HRESULT hr = MFCreateMediaType(&amt);
        if (FAILED(hr)) return hr;
        amt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        amt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        amt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        amt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        amt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        amt->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 128000 / 8);
        amt->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 1024);
        amt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

        // Set AAC codec config (AudioSpecificConfig) if extracted from TS data
        std::vector<uint8_t> audio_config;
        if (demuxer_.GetAudioCodecConfig(audio_config) && !audio_config.empty()) {
            amt->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, audio_config.data(), (UINT32)audio_config.size());
            LOG_INFO("HlsMediaSource: set audio MF_MT_MPEG_SEQUENCE_HEADER (%zu bytes)", audio_config.size());
        }

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

    // MF requires MEStreamCreated on source event queue for each stream
    if (SUCCEEDED(hr) && event_queue_) {
        if (video_stream_)
            event_queue_->QueueEventParamUnk(MEStreamCreated, GUID_NULL, S_OK, video_stream_);
        if (audio_stream_)
            event_queue_->QueueEventParamUnk(MEStreamCreated, GUID_NULL, S_OK, audio_stream_);
        // MESourceInitialized signals the source is ready (before Start)
        event_queue_->QueueEventParamVar(MESourceInitialized, GUID_NULL, S_OK, nullptr);
    }
    return hr;
}

STDMETHODIMP HlsMediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                                     const PROPVARIANT* pvarStartPosition) {
    // Allow restart — MF calls Start after Stop for pipeline restart
    is_started_ = true;

    // MF event sequence in Start(): MEStarted, then MEStreamStarted per stream
    if (event_queue_) {
        event_queue_->QueueEventParamVar(MEStarted, GUID_NULL, S_OK, nullptr);
    }

    // Start each stream — queues MEStreamStarted
    if (video_stream_) video_stream_->Start();
    if (audio_stream_) audio_stream_->Start();

    // Start read thread (restart if already running)
    if (read_thread_) {
        read_running_ = false;
        WaitForSingleObject(read_thread_, 5000);
        CloseHandle(read_thread_);
        read_thread_ = nullptr;
    }
    read_running_ = true;
    read_thread_ = CreateThread(nullptr, 0, ReadThreadProc, this, 0, nullptr);

    return S_OK;
}

STDMETHODIMP HlsMediaSource::Stop() {
    // MF requires MEStreamStopped before MEStopped
    if (video_stream_) video_stream_->Stop();
    if (audio_stream_) audio_stream_->Stop();

    read_running_ = false;
    if (read_thread_) {
        WaitForSingleObject(read_thread_, 5000);
        CloseHandle(read_thread_);
        read_thread_ = nullptr;
    }

    if (event_queue_) {
        event_queue_->QueueEventParamVar(MEStopped, GUID_NULL, S_OK, nullptr);
    }
    return S_OK;
}

STDMETHODIMP HlsMediaSource::Pause() {
    return S_OK;
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
        bool video_ready = !video_stream_ || video_stream_->HasPendingRequest();
        bool audio_ready = !audio_stream_ || audio_stream_->HasPendingRequest();
        if (!video_ready && !audio_ready) {
            Sleep(5);
            continue;
        }

        // Don't block on byte stream — check data availability first.
        // bs->Read() blocks up to 60s when no data is available, which starves MF.
        if (!byte_stream_->HasUnreadData()) {
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

        // Yield briefly to avoid spinning when no frames available
        if (!demuxer_.HasFrames()) {
            Sleep(5);
        }
    }

    // Signal end-of-stream on both streams
    if (video_stream_) video_stream_->SetEos();
    if (audio_stream_) audio_stream_->SetEos();
}

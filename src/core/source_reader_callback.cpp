#define INITGUID
#include "source_reader_callback.h"
#include "player.h"
#include "../audio_out/audio_output.h"
#include "../util/log.h"
#include <cstdlib>
#include <algorithm>

SourceReaderCallback::SourceReaderCallback() {
    InitializeCriticalSection(&lock_);
    idle_event_ = CreateEvent(nullptr, TRUE, TRUE, nullptr);
}

SourceReaderCallback::~SourceReaderCallback() {
    DeleteCriticalSection(&lock_);
    if (idle_event_) CloseHandle(idle_event_);
}

STDMETHODIMP SourceReaderCallback::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFSourceReaderCallback) {
        *ppvObject = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) SourceReaderCallback::AddRef() {
    return (ULONG)ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) SourceReaderCallback::Release() {
    LONG count = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
        delete this;
    }
    return (ULONG)count;
}

// Defensive null-checks after running_ drops to false — in case Stop() timeout
// allowed OnReadSampleImpl to start past the running_ gate.
// Use atomic load to avoid data race with ClearPointers() on the main thread.
#define CHECK_READER(msg) do { if (!self->reader_.load(std::memory_order_acquire)) { LOG_WARN("OnReadSample: reader_ null " msg); return S_OK; } } while(0)
#define CHECK_AO(msg)     do { if (!self->ao_.load(std::memory_order_acquire))     { LOG_WARN("OnReadSample: ao_ null " msg);     return S_OK; } } while(0)
#define CHECK_PLAYER(msg) do { if (!self->player_.load(std::memory_order_acquire)) { LOG_WARN("OnReadSample: player_ null " msg); return S_OK; } } while(0)

STDMETHODIMP SourceReaderCallback::OnEvent(DWORD /*dwStreamIndex*/, IMFMediaEvent* /*pEvent*/) {
    return S_OK;
}

HRESULT SourceReaderCallback::OnReadSampleImpl(SourceReaderCallback* self, HRESULT hrStatus, DWORD dwStreamIndex,
                                 DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample) {
    // If stopped, discard everything (no processing, no re-request)
    if (!self->running_.load(std::memory_order_acquire)) return S_OK;

    // Sample generation: capture at entry and check before re-request to
    // discard stale samples arriving after Flush+RequestRead (HLS live switch).
    LONG gen = self->generation_.load(std::memory_order_acquire);
    (void)gen;

    if (FAILED(hrStatus) || (dwStreamFlags & MF_SOURCE_READERF_ERROR)) {
        LOG_WARN("OnReadSample FAILED stream=%lu hr=0x%08lX flags=0x%08lX",
                 dwStreamIndex, hrStatus, dwStreamFlags);
        // For CURRENTMEDIATYPECHANGED: this is normal during format negotiation
        // with custom IMFMediaSource — re-request to let MF retry
        if (dwStreamFlags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            LOG_INFO("Media type changed on stream %lu, re-requesting", dwStreamIndex);
            IMFSourceReader* r = self->reader_.load(std::memory_order_acquire);
            if (r && self->running_.load(std::memory_order_acquire))
                r->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
            return S_OK;
        }
        // For other errors: post timer to re-request
        Player* p = self->player_.load(std::memory_order_acquire);
        if (p && p->GetHwnd())
            PostMessage(p->GetHwnd(), WM_TIMER,
                        dwStreamIndex == self->video_stream_ ? TIMER_VIDEO_DISPLAY : TIMER_AUDIO_CHECK, 0);
        return S_OK;
    }

    if (dwStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (self->is_live_) {
            // MF's TS demuxer sets an IMMUTABLE internal EOF flag once it
            // sees 0 bytes from the byte stream. ReadSample will return EOF
            // forever after that — re-requesting just loops. Must recreate
            // the source reader to clear MF's internal state.
            if (dwStreamIndex == self->video_stream_)
                self->video_eof_.store(true, std::memory_order_release);
            else if (dwStreamIndex == self->audio_stream_)
                self->audio_eof_.store(true, std::memory_order_release);
            Player* p = self->player_.load(std::memory_order_acquire);
            if (p) p->NotifyLiveEof();
        } else {
            LOG_DEBUG("EOF stream=%lu", dwStreamIndex);
            if (dwStreamIndex == self->video_stream_)
                self->video_eof_.store(true, std::memory_order_release);
            else if (dwStreamIndex == self->audio_stream_)
                self->audio_eof_.store(true, std::memory_order_release);
        }
        return S_OK;
    }

    if (dwStreamFlags & MF_SOURCE_READERF_STREAMTICK) {
        // Gap in the stream — re-request to continue reading
        IMFSourceReader* r = self->reader_.load(std::memory_order_acquire);
        if (r && self->running_.load(std::memory_order_acquire))
            r->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        return S_OK;
    }

    // Handle media type changes (adaptive bitrate)
    if (dwStreamFlags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) {
        LOG_INFO("Native media type changed stream=%lu", dwStreamIndex);
        IMFSourceReader* r = self->reader_.load(std::memory_order_acquire);
        if (r && dwStreamIndex == self->video_stream_) {
            // Re-set our preferred output format
            ComPtr<IMFMediaType> mt;
            if (FAILED(MFCreateMediaType(&mt)) || !mt) {
                LOG_ERROR("MFCreateMediaType failed in type change handler");
                return S_OK;
            }
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            struct { const GUID* fmt; } fmts[] = {
                { &MFVideoFormat_NV12 }, { &MFVideoFormat_I420 },
                { &MFVideoFormat_YUY2 }, { &MFVideoFormat_ARGB32 },
                { &MFVideoFormat_RGB32 },
            };
            for (auto& f : fmts) {
                mt->SetGUID(MF_MT_SUBTYPE, *f.fmt);
                if (SUCCEEDED(r->SetCurrentMediaType(
                        dwStreamIndex, nullptr, mt.get())))
                    break;
            }
        }
    }

    if ((dwStreamFlags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED |
                           MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) &&
        dwStreamIndex == self->video_stream_) {
        Player* p = self->player_.load(std::memory_order_acquire);
        if (p) p->OnVideoFormatChanged();
    }

    if (pSample) {
        if (dwStreamIndex == self->audio_stream_) {
            CHECK_AO("audio sample");
            // Discard stale audio samples from before Flush/Seek
            if (gen != self->generation_.load(std::memory_order_acquire))
                return S_OK;
            AudioOutput* a = self->ao_.load(std::memory_order_acquire);
            if (!a) return S_OK;
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD max_len = 0, cur_len = 0;
                if (SUCCEEDED(buf->Lock(&data, &max_len, &cur_len))) {
                    {
                        int size = (int)(std::min)(cur_len, (DWORD)INT_MAX);
                        double pts_sec = llTimestamp / 10000000.0;
                        int written = a->Write(data, size);
                        // Head PTS: sample start + duration of written data in ring.
                        // last_write_pts_ must track the ring HEAD (newest data) so
                        // GetClock can correctly compute: head_pts - RingAvail / byte_rate.
                        if (written > 0) {
                            int in_bps = a->GetInputByteRate();
                            if (in_bps > 0)
                                pts_sec += (double)written / in_bps;
                            a->SetPts(pts_sec);
                        }
                    }
                    buf->Unlock();
                }
            }
        } else if (dwStreamIndex == self->video_stream_) {
            CHECK_PLAYER("video sample");
            // Discard stale video frames from before Flush/Seek
            if (gen != self->generation_.load(std::memory_order_acquire))
                return S_OK;
            Player* p = self->player_.load(std::memory_order_acquire);
            if (p) p->ProcessVideoFrame(pSample, llTimestamp);
        }
    }

    if (self->running_.load(std::memory_order_acquire)) {
        IMFSourceReader* r = self->reader_.load(std::memory_order_acquire);
        if (r && dwStreamIndex == self->audio_stream_) {
            AudioOutput* a = self->ao_.load(std::memory_order_acquire);
            // Re-request audio when ring buffer has space.
            // Network streams: 64KB threshold (faster refill).
            // Local files: 256KB threshold (less aggressive).
            int free_threshold = self->is_network_ ? 64 * 1024 : 256 * 1024;
            if (a && a->GetFree() > free_threshold)
                r->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        } else if (r && dwStreamIndex == self->video_stream_ && pSample) {
            LONG vp = self->video_pending_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (vp < 3)
                r->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        } else if (r && dwStreamIndex == self->video_stream_ && !pSample &&
                   (dwStreamFlags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED |
                                      MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))) {
            // Type change without a sample — re-request to keep pipeline primed
            r->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        } else if (r && dwStreamIndex == self->video_stream_ && !pSample &&
                   !(dwStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            // NULL sample without EOF or type change — re-request to prevent pipeline stall
            r->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        }
    }

    return S_OK;
}

STDMETHODIMP SourceReaderCallback::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex,
                                                  DWORD dwStreamFlags, LONGLONG llTimestamp,
                                                  IMFSample* pSample) {
    AddRef();  // keep self alive — caller may Release while we're busy
    busy_.fetch_add(1, std::memory_order_acq_rel);
    if (idle_event_) ResetEvent(idle_event_);
    HRESULT hr;
    __try {
        hr = OnReadSampleImpl(this, hrStatus, dwStreamIndex, dwStreamFlags, llTimestamp, pSample);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("OnReadSample SEH: 0x%08lX", GetExceptionCode());
        hr = E_FAIL;
    }
    if (busy_.fetch_sub(1, std::memory_order_release) == 1 && idle_event_)
        SetEvent(idle_event_);
    Release();
    return hr;
}

STDMETHODIMP SourceReaderCallback::OnFlush(DWORD /*dwStreamIndex*/) {
    return S_OK;
}

void SourceReaderCallback::SetReader(IMFSourceReader* reader,
                                       DWORD video_stream, DWORD audio_stream) {
    reader_.store(reader, std::memory_order_release);
    video_stream_ = video_stream;
    audio_stream_ = audio_stream;
}

HRESULT SourceReaderCallback::StartReading() {
    IMFSourceReader* r = reader_.load(std::memory_order_acquire);
    if (!r) return E_FAIL;

    EnterCriticalSection(&lock_);
    running_.store(true, std::memory_order_release);
    video_eof_.store(false, std::memory_order_release);
    audio_eof_.store(false, std::memory_order_release);
    video_pending_.store(0, std::memory_order_relaxed);
    LeaveCriticalSection(&lock_);

    if (video_stream_ != (DWORD)-1) {
        HRESULT hr = r->ReadSample(video_stream_, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) LOG_WARN("ReadSample video failed: 0x%08lX", hr);
    }
    if (audio_stream_ != (DWORD)-1) {
        HRESULT hr = r->ReadSample(audio_stream_, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) LOG_WARN("ReadSample audio failed: 0x%08lX", hr);
    }
    return S_OK;
}

HRESULT SourceReaderCallback::Stop() {
    EnterCriticalSection(&lock_);
    running_.store(false, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_release);
    LeaveCriticalSection(&lock_);
    // Wait for any in-flight OnReadSample to finish before caller deletes objects.
    // OnReadSample processes a single sample and returns — 1s is generous.
    if (busy_.load(std::memory_order_acquire) > 0 && idle_event_) {
        DWORD wr = WaitForSingleObject(idle_event_, 1000);
        if (wr == WAIT_TIMEOUT)
            LOG_WARN("Stop: OnReadSample still busy after 1s");
    }
    return S_OK;
}

HRESULT SourceReaderCallback::RequestVideoRead() {
    IMFSourceReader* r = reader_.load(std::memory_order_acquire);
    if (!r || video_stream_ == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&lock_);
    bool ok = running_.load(std::memory_order_relaxed) && !video_eof_.load(std::memory_order_relaxed);
    LeaveCriticalSection(&lock_);
    if (!ok) return E_FAIL;
    return r->ReadSample(video_stream_, 0, nullptr, nullptr, nullptr, nullptr);
}

HRESULT SourceReaderCallback::RequestAudioRead() {
    IMFSourceReader* r = reader_.load(std::memory_order_acquire);
    if (!r || audio_stream_ == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&lock_);
    bool ok = running_.load(std::memory_order_relaxed) && !audio_eof_.load(std::memory_order_relaxed);
    LeaveCriticalSection(&lock_);
    if (!ok) return E_FAIL;
    return r->ReadSample(audio_stream_, 0, nullptr, nullptr, nullptr, nullptr);
}

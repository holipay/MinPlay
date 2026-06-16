#define INITGUID
#include "source_reader_callback.h"
#include "player.h"
#include "../audio_out/audio_output.h"
#include "../util/log.h"
#include <cstdlib>
#include <algorithm>

SourceReaderCallback::SourceReaderCallback() {
    InitializeCriticalSection(&lock_);
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
        DeleteCriticalSection(&lock_);
        delete this;
    }
    return (ULONG)count;
}

STDMETHODIMP SourceReaderCallback::OnEvent(DWORD /*dwStreamIndex*/, IMFMediaEvent* /*pEvent*/) {
    return S_OK;
}

HRESULT SourceReaderCallback::OnReadSampleImpl(SourceReaderCallback* self, HRESULT hrStatus, DWORD dwStreamIndex,
                                 DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample) {
    // If stopped, discard everything (no processing, no re-request)
    if (!self->running_.load(std::memory_order_acquire)) return S_OK;

    if (FAILED(hrStatus) || (dwStreamFlags & MF_SOURCE_READERF_ERROR)) {
        LOG_WARN("OnReadSample failed stream=%lu hr=0x%08lX flags=0x%08lX",
                 dwStreamIndex, hrStatus, dwStreamFlags);
        if (self->reader_) {
            self->reader_->Flush(dwStreamIndex);
        }
        if (self->player_ && self->player_->GetHwnd())
            PostMessage(self->player_->GetHwnd(), WM_TIMER,
                        dwStreamIndex == self->video_stream_ ? TIMER_VIDEO_DISPLAY : TIMER_AUDIO_CHECK, 0);
        return S_OK;
    }

    if (dwStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (dwStreamIndex == self->video_stream_)
            self->video_eof_ = true;
        else if (dwStreamIndex == self->audio_stream_)
            self->audio_eof_ = true;
        LOG_INFO("EOF stream=%lu", dwStreamIndex);
        return S_OK;
    }

    if (dwStreamFlags & MF_SOURCE_READERF_STREAMTICK) {
        // Gap in the stream — re-request to continue reading
        if (self->running_.load(std::memory_order_acquire) && self->reader_)
            self->reader_->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        return S_OK;
    }

    // Handle media type changes (adaptive bitrate)
    if (dwStreamFlags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) {
        LOG_INFO("Native media type changed stream=%lu", dwStreamIndex);
        if (dwStreamIndex == self->video_stream_ && self->reader_) {
            // Re-set our preferred output format
            ComPtr<IMFMediaType> mt;
            MFCreateMediaType(&mt);
            mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            struct { const GUID* fmt; } fmts[] = {
                { &MFVideoFormat_NV12 }, { &MFVideoFormat_I420 },
                { &MFVideoFormat_YUY2 }, { &MFVideoFormat_ARGB32 },
                { &MFVideoFormat_RGB32 },
            };
            for (auto& f : fmts) {
                mt->SetGUID(MF_MT_SUBTYPE, *f.fmt);
                if (SUCCEEDED(self->reader_->SetCurrentMediaType(
                        dwStreamIndex, nullptr, mt.get())))
                    break;
            }
        }
    }

    if (dwStreamFlags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED |
                         MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)) {
        if (self->player_) self->player_->OnVideoFormatChanged();
    }

    if (pSample) {
        if (dwStreamIndex == self->audio_stream_ && self->ao_) {
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD max_len = 0, cur_len = 0;
                if (SUCCEEDED(buf->Lock(&data, &max_len, &cur_len))) {
                    self->ao_->Write(data, (int)(std::min)(cur_len, (DWORD)INT_MAX));
                    self->ao_->SetPts(llTimestamp / 10000000.0);
                    buf->Unlock();
                }
            }
        } else if (dwStreamIndex == self->video_stream_ && self->player_) {
            self->player_->ProcessVideoFrame(pSample, llTimestamp);
        }
    }

    if (self->running_.load(std::memory_order_acquire) && self->reader_) {
        if (dwStreamIndex == self->audio_stream_ && self->ao_) {
            if (self->ao_->GetFree() > 256 * 1024)
                self->reader_->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        } else if (dwStreamIndex == self->video_stream_ && pSample) {
            LONG vp = self->video_pending_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (vp < 2)
                self->reader_->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        } else if (dwStreamIndex == self->video_stream_ && !pSample &&
                   (dwStreamFlags & (MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED |
                                     MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))) {
            // Type change without a sample — re-request to keep pipeline primed
            self->reader_->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        }
    }

    return S_OK;
}

STDMETHODIMP SourceReaderCallback::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex,
                                                  DWORD dwStreamFlags, LONGLONG llTimestamp,
                                                  IMFSample* pSample) {
    busy_.fetch_add(1, std::memory_order_acq_rel);
    HRESULT hr;
    __try {
        hr = OnReadSampleImpl(this, hrStatus, dwStreamIndex, dwStreamFlags, llTimestamp, pSample);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("OnReadSample SEH: 0x%08lX", GetExceptionCode());
        hr = S_OK;
    }
    busy_.fetch_sub(1, std::memory_order_release);
    return hr;
}

STDMETHODIMP SourceReaderCallback::OnFlush(DWORD /*dwStreamIndex*/) {
    return S_OK;
}

void SourceReaderCallback::SetReader(IMFSourceReader* reader,
                                      DWORD video_stream, DWORD audio_stream) {
    reader_ = reader;
    video_stream_ = video_stream;
    audio_stream_ = audio_stream;
}

HRESULT SourceReaderCallback::StartReading() {
    if (!reader_) return E_FAIL;

    EnterCriticalSection(&lock_);
    running_.store(true, std::memory_order_release);
    video_eof_.store(false, std::memory_order_release);
    audio_eof_.store(false, std::memory_order_release);
    video_pending_.store(0, std::memory_order_relaxed);
    LeaveCriticalSection(&lock_);

    if (video_stream_ != (DWORD)-1) {
        HRESULT hr = reader_->ReadSample(video_stream_, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) LOG_WARN("ReadSample video failed: 0x%08lX", hr);
    }
    if (audio_stream_ != (DWORD)-1) {
        HRESULT hr = reader_->ReadSample(audio_stream_, 0, nullptr, nullptr, nullptr, nullptr);
        if (FAILED(hr)) LOG_WARN("ReadSample audio failed: 0x%08lX", hr);
    }
    return S_OK;
}

HRESULT SourceReaderCallback::Stop() {
    EnterCriticalSection(&lock_);
    running_.store(false, std::memory_order_release);
    LeaveCriticalSection(&lock_);
    // Wait for any in-flight OnReadSample to finish before caller deletes objects
    while (busy_.load(std::memory_order_acquire) > 0)
        SwitchToThread();
    return S_OK;
}

HRESULT SourceReaderCallback::RequestVideoRead() {
    if (!reader_ || video_stream_ == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&lock_);
    bool ok = running_.load(std::memory_order_relaxed) && !video_eof_.load(std::memory_order_relaxed);
    LeaveCriticalSection(&lock_);
    if (!ok) return E_FAIL;
    return reader_->ReadSample(video_stream_, 0, nullptr, nullptr, nullptr, nullptr);
}

HRESULT SourceReaderCallback::RequestAudioRead() {
    if (!reader_ || audio_stream_ == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&lock_);
    bool ok = running_.load(std::memory_order_relaxed) && !audio_eof_.load(std::memory_order_relaxed);
    LeaveCriticalSection(&lock_);
    if (!ok) return E_FAIL;
    return reader_->ReadSample(audio_stream_, 0, nullptr, nullptr, nullptr, nullptr);
}

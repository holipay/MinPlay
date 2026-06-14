#define INITGUID
#include "source_reader_callback.h"
#include "player.h"
#include "../audio_out/audio_output.h"
#include "../util/log.h"
#include <cstdlib>

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
    return InterlockedIncrement(&ref_count_);
}

STDMETHODIMP_(ULONG) SourceReaderCallback::Release() {
    LONG count = InterlockedDecrement(&ref_count_);
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
    if (FAILED(hrStatus) || !self->running_ || (dwStreamFlags & MF_SOURCE_READERF_ERROR)) {
        if (dwStreamFlags & MF_SOURCE_READERF_ERROR)
            LOG_ERROR("OnReadSample error stream=%lu", dwStreamIndex);
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

    if (pSample) {
        if (dwStreamIndex == self->audio_stream_ && self->ao_) {
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD max_len = 0, cur_len = 0;
                buf->Lock(&data, &max_len, &cur_len);
                self->ao_->Write(data, (int)cur_len);
                self->ao_->SetPts(llTimestamp / 10000000.0);
                buf->Unlock();
            }
        } else if (dwStreamIndex == self->video_stream_ && self->player_) {
            self->player_->ProcessVideoFrame(pSample, llTimestamp);
        }
    }

    if (self->running_ && self->reader_) {
        if (dwStreamIndex == self->audio_stream_ && self->ao_) {
            if (self->ao_->GetFree() > 256 * 1024)
                self->reader_->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        } else if (dwStreamIndex == self->video_stream_) {
            LONG vp = InterlockedIncrement(&self->video_pending_);
            if (vp < 2)
                self->reader_->ReadSample(dwStreamIndex, 0, nullptr, nullptr, nullptr, nullptr);
        }
    }

    return S_OK;
}

STDMETHODIMP SourceReaderCallback::OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex,
                                                  DWORD dwStreamFlags, LONGLONG llTimestamp,
                                                  IMFSample* pSample) {
    __try {
        return OnReadSampleImpl(this, hrStatus, dwStreamIndex, dwStreamFlags, llTimestamp, pSample);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("OnReadSample SEH: 0x%08lX", GetExceptionCode());
        return S_OK;
    }
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
    running_ = true;
    video_eof_ = false;
    audio_eof_ = false;
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
    running_ = false;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

HRESULT SourceReaderCallback::RequestVideoRead() {
    if (!reader_ || video_stream_ == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&lock_);
    bool ok = running_ && !video_eof_;
    LeaveCriticalSection(&lock_);
    if (!ok) return E_FAIL;
    return reader_->ReadSample(video_stream_, 0, nullptr, nullptr, nullptr, nullptr);
}

HRESULT SourceReaderCallback::RequestAudioRead() {
    if (!reader_ || audio_stream_ == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&lock_);
    bool ok = running_ && !audio_eof_;
    LeaveCriticalSection(&lock_);
    if (!ok) return E_FAIL;
    return reader_->ReadSample(audio_stream_, 0, nullptr, nullptr, nullptr, nullptr);
}

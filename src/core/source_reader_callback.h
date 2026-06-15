#pragma once
#include "../util/com_ptr.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

class Player;

class SourceReaderCallback : public IMFSourceReaderCallback {
public:
    SourceReaderCallback();
    virtual ~SourceReaderCallback() = default;

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFSourceReaderCallback
    STDMETHODIMP OnEvent(DWORD dwStreamIndex, IMFMediaEvent* pEvent) override;
    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD dwStreamIndex,
                              DWORD dwStreamFlags, LONGLONG llTimestamp,
                              IMFSample* pSample) override;
    STDMETHODIMP OnFlush(DWORD dwStreamIndex) override;

    static HRESULT OnReadSampleImpl(SourceReaderCallback* self, HRESULT hrStatus, DWORD dwStreamIndex,
                                     DWORD dwStreamFlags, LONGLONG llTimestamp, IMFSample* pSample);

    void SetReader(IMFSourceReader* reader, DWORD video_stream, DWORD audio_stream);
    void SetPlayer(Player* player) { player_ = player; }
    void SetAudioOutput(class AudioOutput* ao) { ao_ = ao; }

    HRESULT StartReading();
    HRESULT Stop();
    HRESULT RequestVideoRead();
    HRESULT RequestAudioRead();

    bool IsVideoEof() const { return video_eof_; }
    bool IsAudioEof() const { return audio_eof_; }
    void ResetVideoEof() { video_eof_ = false; }
    void ResetAudioEof() { audio_eof_ = false; }
    void ConsumeVideo() { InterlockedDecrement(&video_pending_); }

private:
    volatile LONG ref_count_ = 1;

    IMFSourceReader* reader_ = nullptr;
    DWORD video_stream_ = (DWORD)-1;
    DWORD audio_stream_ = (DWORD)-1;
    AudioOutput* ao_ = nullptr;
    Player* player_ = nullptr;

    CRITICAL_SECTION lock_;
    volatile bool running_ = false;
    volatile bool video_eof_ = false;
    volatile bool audio_eof_ = false;
    volatile LONG video_pending_ = 0;
};

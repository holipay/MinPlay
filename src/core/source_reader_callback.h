#pragma once
#include "../util/com_ptr.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>

class Player;

class SourceReaderCallback : public IMFSourceReaderCallback {
public:
    SourceReaderCallback();
    virtual ~SourceReaderCallback();

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
    void SetPlayer(Player* player) { player_.store(player, std::memory_order_release); }
    void SetAudioOutput(class AudioOutput* ao) { ao_.store(ao, std::memory_order_release); }
    void SetLive(bool live) { is_live_ = live; }
    void ClearPointers() {
        reader_.store(nullptr, std::memory_order_release);
        ao_.store(nullptr, std::memory_order_release);
        player_.store(nullptr, std::memory_order_release);
    }

    HRESULT StartReading();
    HRESULT Stop();
    HRESULT RequestVideoRead();
    HRESULT RequestAudioRead();

    bool IsVideoEof() const { return video_eof_.load(std::memory_order_acquire); }
    bool IsAudioEof() const { return audio_eof_.load(std::memory_order_acquire); }
    void ResetVideoEof() { video_eof_.store(false, std::memory_order_release); }
    void ResetAudioEof() { audio_eof_.store(false, std::memory_order_release); }
    void ConsumeVideo() { video_pending_.fetch_sub(1, std::memory_order_relaxed); }
    bool IsBusy() const { return busy_.load(std::memory_order_acquire) > 0; }
    LONG GetGeneration() const { return generation_.load(std::memory_order_acquire); }

private:
    std::atomic<LONG> ref_count_{1};

    std::atomic<IMFSourceReader*> reader_{nullptr};
    DWORD video_stream_ = (DWORD)-1;
    DWORD audio_stream_ = (DWORD)-1;
    std::atomic<AudioOutput*> ao_{nullptr};
    std::atomic<Player*> player_{nullptr};

    CRITICAL_SECTION lock_;
    std::atomic<bool> running_{false};
    std::atomic<bool> video_eof_{false};
    std::atomic<bool> audio_eof_{false};
    std::atomic<LONG> video_pending_{0};
    std::atomic<LONG> busy_{0};
    HANDLE idle_event_ = nullptr;  // signaled when busy_ == 0
    std::atomic<LONG> generation_{0};
    bool is_live_ = false;
};

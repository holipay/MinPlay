#pragma once
#include "../util/com_ptr.h"
#include <atomic>
#include <windows.h>
#include <mfidl.h>
#include <mfapi.h>
#include <queue>
#include <vector>

class HlsMediaSource;

/*
 * HlsMediaStream: Minimal IMFMediaStream implementation for HlsMediaSource.
 * Delivers demuxed ES frames to MF's source reader as IMFSamples.
 */
class HlsMediaStream : public IMFMediaStream {
public:
    HlsMediaStream(HlsMediaSource* source, DWORD stream_id,
                   IMFStreamDescriptor* sd);
    ~HlsMediaStream();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    // Called by HlsMediaSource::ReadLoop to deliver a frame
    void DeliverFrame(const uint8_t* data, size_t size, double pts_sec);

    // Stream lifecycle
    void Start();
    void Stop();

    // Mark stream as ended
    void SetEos();

    bool HasPendingRequest() const {
        // Note: caller must hold token_lock_ for thread safety
        return !tokens_.empty();
    }

private:
    HlsMediaSource* source_;
    DWORD stream_id_;
    IMFStreamDescriptor* stream_desc_;
    IMFMediaEventQueue* event_queue_;

    std::atomic<ULONG> ref_count_{1};
    std::queue<ComPtr<IUnknown>> tokens_;
    CRITICAL_SECTION token_lock_;
    bool is_started_ = false;
};

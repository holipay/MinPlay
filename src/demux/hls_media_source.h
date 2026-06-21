#pragma once
#include "../demux/ts_demuxer.h"
#include "../util/com_ptr.h"
#include <atomic>
#include <vector>
#include <queue>
#include <mutex>
#include <windows.h>
#include <mfidl.h>
#include <mfapi.h>

class HlsByteStream;

/*
 * HlsMediaSource: Custom IMFMediaSource for HLS live streams.
 *
 * Replaces MF's TS demuxer to eliminate the immutable EOF flag issue.
 * Reads TS data from HlsByteStream, parses with TsDemuxer, and provides
 * raw ES data (H.264/AAC) to MF's decoders.
 *
 * Key design: never signals EOF for live streams. The byte stream's
 * BeginRead blocks until new data arrives, preventing MF from setting
 * its immutable internal EOF flag.
 */
class HlsMediaSource : public IMFMediaSource {
public:
    static HRESULT CreateInstance(HlsByteStream* byte_stream, IMFMediaSource** ppSource);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                       const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // Called by internal threads to deliver samples
    void DeliverVideoSample(const uint8_t* data, int size, double pts);
    void DeliverAudioSample(const uint8_t* data, int size, double pts);

private:
    HlsMediaSource(HlsByteStream* byte_stream);
    ~HlsMediaSource();

    HlsByteStream* byte_stream_;
    TsDemuxer demuxer_;

    std::atomic<ULONG> ref_count_{1};
    CRITICAL_SECTION lock_;
    bool is_started_;
    bool is_shutdown_;

    // Sample queue
    struct QueuedSample {
        DWORD stream_index;
        IMFSample* sample;
        double pts;
    };
    std::queue<QueuedSample> sample_queue_;
    CRITICAL_SECTION queue_lock_;

    // Read thread
    HANDLE read_thread_;
    bool read_running_;

    static DWORD WINAPI ReadThreadProc(LPVOID param);
    void ReadLoop();
};

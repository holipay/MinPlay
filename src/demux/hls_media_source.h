#pragma once
#include "../demux/ts_demuxer.h"
#include "../util/com_ptr.h"
#include <atomic>
#include <vector>
#include <windows.h>
#include <mfidl.h>
#include <mfapi.h>

class HlsByteStream;
class HlsMediaStream;

/*
 * HlsMediaSource: Custom IMFMediaSourceEx for HLS live streams.
 *
 * Implements all interfaces required by MFCreateSourceReaderFromMediaSource:
 *   IMFMediaSource, IMFMediaSourceEx, IMFMediaEventGenerator,
 *   IMFGetService, IMFQualityAdvise, IMFMediaSourceAttributes
 */
class HlsMediaSource : public IMFMediaSourceEx,
                        public IMFGetService {
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

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** ppAttributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* pManager) override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID guidService, REFIID riid, void** ppvObject) override;

    HlsByteStream* GetByteStream() const { return byte_stream_; }

private:
    HlsMediaSource(HlsByteStream* byte_stream);
    ~HlsMediaSource();

    HlsByteStream* byte_stream_;
    TsDemuxer demuxer_;

    std::atomic<ULONG> ref_count_{1};
    CRITICAL_SECTION lock_;
    bool is_started_;
    bool is_shutdown_;

    IMFMediaEventQueue* event_queue_;

    HANDLE read_thread_;
    bool read_running_;

    HlsMediaStream* video_stream_;
    HlsMediaStream* audio_stream_;

    // Source attributes (MF_QUERYSERVICE)
    IMFAttributes* source_attrs_;

    static DWORD WINAPI ReadThreadProc(LPVOID param);
    void ReadLoop();
};

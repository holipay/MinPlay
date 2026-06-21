#pragma once
#include "../demux/ts_demuxer.h"
#include "../util/com_ptr.h"
#include <atomic>
#include <vector>
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
 * raw ES data to MF's decoders.
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

    // IMFMediaEventGenerator (required by IMFMediaSource)
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* pState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                            HRESULT hrStatus, const PROPVARIANT* pvValue) override;

private:
    HlsMediaSource(HlsByteStream* byte_stream);
    ~HlsMediaSource();

    HlsByteStream* byte_stream_;
    TsDemuxer demuxer_;

    std::atomic<ULONG> ref_count_{1};
    CRITICAL_SECTION lock_;
    bool is_started_;
    bool is_shutdown_;
};

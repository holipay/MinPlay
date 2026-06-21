#pragma once
#include "../demux/ts_demuxer.h"
#include "../util/com_ptr.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <windows.h>
#include <mfidl.h>

// Forward declarations
class HlsByteStream;

/*
 * TsByteStream: Custom IMFByteStream that wraps HlsByteStream + TsDemuxer.
 *
 * Reads raw TS data from HlsByteStream, parses it with TsDemuxer,
 * and provides raw ES data (H.264 NAL units / AAC ADTS frames) to MF.
 *
 * This replaces MF's TS demuxer (msdatmpg.dll) to eliminate the
 * immutable EOF flag issue that causes HLS live stream stuttering.
 */
class TsByteStream : public IMFByteStream {
public:
    TsByteStream(HlsByteStream* hls_stream);
    virtual ~TsByteStream();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFByteStream
    STDMETHODIMP GetCapabilities(DWORD* pdwCapabilities) override;
    STDMETHODIMP GetLength(QWORD* pqwLength) override;
    STDMETHODIMP SetLength(QWORD) override;
    STDMETHODIMP GetCurrentPosition(QWORD* pqwPosition) override;
    STDMETHODIMP SetCurrentPosition(QWORD qwPosition) override;
    STDMETHODIMP IsEndOfStream(BOOL* pfEndOfStream) override;
    STDMETHODIMP Read(BYTE* pb, ULONG cb, ULONG* pcbRead) override;
    STDMETHODIMP BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* pCallback, IUnknown* pState) override;
    STDMETHODIMP EndRead(IMFAsyncResult* pResult, ULONG* pcbRead) override;
    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override;
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override;
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override;
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN SeekOrigin, LONGLONG llSeekOffset,
                      DWORD dwSeekFlags, QWORD* pqwCurrentPosition) override;
    STDMETHODIMP Flush() override;
    STDMETHODIMP Close() override;

    // Get stream info for media type setup
    bool HasVideo() const { return has_video_; }
    bool HasAudio() const { return has_audio_; }
    int GetVideoWidth() const { return video_width_; }
    int GetVideoHeight() const { return video_height_; }
    int GetAudioSampleRate() const { return audio_sample_rate_; }
    int GetAudioChannels() const { return audio_channels_; }

    // Abort pending reads for shutdown
    void Abort();

private:
    HlsByteStream* hls_stream_;
    TsDemuxer demuxer_;

    std::atomic<ULONG> ref_count_{1};
    CRITICAL_SECTION lock_;
    HANDLE data_event_;
    bool closed_;

    // Output buffer (raw ES data from TsDemuxer)
    std::vector<uint8_t> output_buffer_;
    int output_pos_;
    int output_size_;

    // Async result for EndRead
    std::atomic<ULONG> async_result_{0};
    std::atomic<HRESULT> async_hr_{E_ABORT};

    // Stream info (populated after first parse)
    bool has_video_;
    bool has_audio_;
    int video_width_;
    int video_height_;
    int audio_sample_rate_;
    int audio_channels_;

    // Refill output buffer from TsDemuxer
    bool RefillBuffer();
};

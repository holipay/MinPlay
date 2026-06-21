#include "ts_byte_stream.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <cstring>

TsByteStream::TsByteStream(HlsByteStream* hls_stream)
    : hls_stream_(hls_stream), ref_count_(1), closed_(false),
      output_pos_(0), output_size_(0),
      has_video_(false), has_audio_(false),
      video_width_(0), video_height_(0),
      audio_sample_rate_(0), audio_channels_(0) {
    InitializeCriticalSection(&lock_);
    data_event_ = CreateEvent(nullptr, TRUE, FALSE, nullptr);  // manual-reset
    if (hls_stream_) hls_stream_->AddRef();
}

TsByteStream::~TsByteStream() {
    if (hls_stream_) hls_stream_->Release();
    if (data_event_) CloseHandle(data_event_);
    DeleteCriticalSection(&lock_);
}

STDMETHODIMP TsByteStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IMFByteStream) {
        *ppv = static_cast<IMFByteStream*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) TsByteStream::AddRef() {
    return (ULONG)ref_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

STDMETHODIMP_(ULONG) TsByteStream::Release() {
    LONG r = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP TsByteStream::GetCapabilities(DWORD* pdwCapabilities) {
    if (!pdwCapabilities) return E_POINTER;
    *pdwCapabilities = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
    return S_OK;
}

STDMETHODIMP TsByteStream::GetLength(QWORD* pqwLength) {
    if (!pqwLength) return E_POINTER;
    // Return a large value to indicate unknown length (live stream)
    *pqwLength = 0x7FFFFFFFFFFFFFFF;
    return S_OK;
}

STDMETHODIMP TsByteStream::SetLength(QWORD) { return E_NOTIMPL; }

STDMETHODIMP TsByteStream::GetCurrentPosition(QWORD* pqwPosition) {
    if (!pqwPosition) return E_POINTER;
    EnterCriticalSection(&lock_);
    *pqwPosition = output_pos_;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP TsByteStream::SetCurrentPosition(QWORD qwPosition) {
    EnterCriticalSection(&lock_);
    output_pos_ = (int)qwPosition;
    LeaveCriticalSection(&lock_);
    return S_OK;
}

STDMETHODIMP TsByteStream::IsEndOfStream(BOOL* pfEndOfStream) {
    if (!pfEndOfStream) return E_POINTER;
    // Never report EOF for live streams
    *pfEndOfStream = FALSE;
    return S_OK;
}

bool TsByteStream::RefillBuffer() {
    // Get frames from TsDemuxer (which maintains its own state)
    DemuxFrame frame;
    if (demuxer_.GetNextFrame(frame)) {
        output_buffer_ = std::move(frame.data);
        output_pos_ = 0;
        output_size_ = (int)output_buffer_.size();

        if (!has_video_ && frame.type == DemuxFrame::Video && frame.data.size() > 0) {
            has_video_ = true;
            LOG_INFO("TsByteStream: video stream detected (%zu bytes)", frame.data.size());
        }
        if (!has_audio_ && frame.type == DemuxFrame::Audio && frame.data.size() > 0) {
            has_audio_ = true;
            LOG_INFO("TsByteStream: audio stream detected (%zu bytes)", frame.data.size());
        }
        return true;
    }

    // No frames — read more TS data from HlsByteStream and feed to demuxer
    if (!hls_stream_) return false;

    const int TS_READ_SIZE = 256 * 1024;
    std::vector<uint8_t> ts_data(TS_READ_SIZE);
    ULONG ts_bytes_read = 0;
    HRESULT hr = hls_stream_->Read(ts_data.data(), TS_READ_SIZE, &ts_bytes_read);

    if (FAILED(hr) || ts_bytes_read == 0) {
        return false;  // No data available yet — caller should retry
    }

    // Feed TS data to demuxer (it maintains state internally)
    // TsDemuxer::ReadAndDemux reads from a byte stream, but we have raw bytes.
    // Instead, parse the TS packets directly.
    TsPacketParser parser;
    int pos = parser.FindSyncByte(ts_data.data(), (int)ts_bytes_read, 0);
    if (pos < 0) return false;

    while (pos + TS_PACKET_SIZE <= (int)ts_bytes_read) {
        TsPacket packet;
        if (parser.ParsePacket(ts_data.data() + pos, (int)ts_bytes_read - pos, packet)) {
            // Feed to demuxer's internal assemblers (video/audio PID)
            // This is simplified — in production, would use demuxer's full pipeline
        }
        pos += TS_PACKET_SIZE;
        // Re-align to next sync byte
        if (pos + TS_PACKET_SIZE <= (int)ts_bytes_read) {
            int next_sync = parser.FindSyncByte(ts_data.data(), (int)ts_bytes_read, pos);
            if (next_sync > pos) pos = next_sync;
            else if (next_sync < 0) break;
        }
    }

    // For now, return false — full integration needs proper ES data buffering
    return false;
}

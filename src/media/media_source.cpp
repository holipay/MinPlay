#include "media_source.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <propvarutil.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

static const GUID GUID_NULL_VAL = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};

HRESULT MediaSource::GetUint64Pair(IMFAttributes* attr, REFGUID key,
                                    UINT32* a, UINT32* b) {
    UINT64 val = 0;
    HRESULT hr = attr->GetUINT64(key, &val);
    if (SUCCEEDED(hr)) {
        *a = (UINT32)(val >> 32);
        *b = (UINT32)(val & 0xFFFFFFFF);
    }
    return hr;
}

bool MediaSource::Open(const wchar_t* url, IMFSourceReaderCallback* callback) {
    ComPtr<IMFAttributes> sattrs;
    HRESULT hr = MFCreateAttributes(&sattrs, callback ? 3 : 2);
    if (FAILED(hr)) return false;

    sattrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (callback) {
        sattrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback);
    }

    // Check if URL is HTTP(S) — may need HLS path
    bool is_http = (wcsncmp(url, L"http://", 7) == 0) ||
                   (wcsncmp(url, L"https://", 8) == 0);

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(url, sattrs.get(), &reader);
    sattrs.reset();
    if (FAILED(hr)) {
        if (is_http) {
            LOG_WARN("MFCreateSourceReaderFromURL failed: 0x%08lX — trying HLS path", hr);
            reader.reset();
            if (OpenHls(url)) {
                // Re-create source reader attributes
                hr = MFCreateAttributes(&sattrs, callback ? 2 : 1);
                if (FAILED(hr)) return false;
                if (callback) {
                    sattrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback);
                }
                hr = MFCreateSourceReaderFromByteStream(
                    hls_->GetByteStream(), sattrs.get(), &reader);
                sattrs.reset();
                if (FAILED(hr)) {
                    LOG_ERROR("MFCreateSourceReaderFromByteStream failed: 0x%08lX", hr);
                    return false;
                }
            } else {
                LOG_ERROR("HLS path also failed");
                return false;
            }
        } else {
            LOG_ERROR("MFCreateSourceReaderFromURL failed: 0x%08lX", hr);
            return false;
        }
    }

    ComPtr<IMFMediaType> vmt;
    MFCreateMediaType(&vmt);
    vmt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

    struct { const GUID* fmt; PixelFormat pf; } fmts[] = {
        { &MFVideoFormat_NV12,   PixelFormat::NV12  },
        { &MFVideoFormat_I420,   PixelFormat::I420  },
        { &MFVideoFormat_YUY2,   PixelFormat::YUY2  },
        { &MFVideoFormat_ARGB32, PixelFormat::RGB32 },
        { &MFVideoFormat_RGB32,  PixelFormat::RGB32 },
    };

    for (auto& f : fmts) {
        vmt->SetGUID(MF_MT_SUBTYPE, *f.fmt);
        hr = reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, vmt.get());
        if (SUCCEEDED(hr)) {
            has_video_ = true;
            pix_fmt_ = f.pf;

            ComPtr<IMFMediaType> native;
            reader->GetCurrentMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, &native);
            UINT32 w = 0, h = 0, num = 0, den = 0;
            GetUint64Pair(native.get(), MF_MT_FRAME_SIZE, &w, &h);
            GetUint64Pair(native.get(), MF_MT_FRAME_RATE, &num, &den);
            vi_.width = (int)w;
            vi_.height = (int)h;
            vi_.fps = den ? (double)num / den : 30.0;
            LOG_INFO("Video: %dx%d @ %.1f fps (fmt=%d)", w, h, vi_.fps, (int)pix_fmt_);
            break;
        }
    }
    vmt.reset();

    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    ComPtr<IMFMediaType> amt;
    MFCreateMediaType(&amt);
    amt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    amt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    amt->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    amt->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    amt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);

    hr = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, amt.get());
    amt.reset();
    if (SUCCEEDED(hr)) {
        has_audio_ = true;
        ai_.sample_rate = 44100;
        ai_.channels = 2;
        ai_.bits_per_sample = 16;
        LOG_INFO("Audio: 44100 Hz, 2 ch, 16 bit");
    }

    PROPVARIANT dur;
    PropVariantInit(&dur);
    hr = reader->GetPresentationAttribute(
        MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur);
    if (SUCCEEDED(hr) && (dur.vt == VT_I8 || dur.vt == VT_UI8))
        duration_ = dur.hVal.QuadPart / 10000000.0;
    PropVariantClear(&dur);
    LOG_INFO("Duration: %.1f s", duration_);

    video_stream_ = (DWORD)-1;
    audio_stream_ = (DWORD)-1;
    for (DWORD i = 0; i < 16; i++) {
        ComPtr<IMFMediaType> stmtype;
        hr = reader->GetNativeMediaType(i, 0, &stmtype);
        if (FAILED(hr)) break;
        GUID maj;
        stmtype->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Video && video_stream_ == (DWORD)-1) {
            video_stream_ = i;
        } else if (maj == MFMediaType_Audio && audio_stream_ == (DWORD)-1) {
            audio_stream_ = i;
        }
        if (video_stream_ != (DWORD)-1 && audio_stream_ != (DWORD)-1) break;
    }
    LOG_INFO("Streams: video=%lu audio=%lu", video_stream_, audio_stream_);

    reader_ = std::move(reader);
    return true;
}

void MediaSource::ReconfigureVideo() {
    if (!reader_ || video_stream_ == (DWORD)-1) return;
    ComPtr<IMFMediaType> mt;
    if (SUCCEEDED(reader_->GetCurrentMediaType(video_stream_, &mt))) {
        UINT32 w = 0, h = 0, num = 0, den = 0;
        GetUint64Pair(mt.get(), MF_MT_FRAME_SIZE, &w, &h);
        GetUint64Pair(mt.get(), MF_MT_FRAME_RATE, &num, &den);
        // Only update if resolution actually changed (ignore stride padding)
        if (w > 0 && h > 0 &&
            (abs((int)w - vi_.width) > 16 || abs((int)h - vi_.height) > 16)) {
            vi_.width = (int)w;
            vi_.height = (int)h;
        }
        if (den > 0) vi_.fps = (double)num / den;

        GUID subtype;
        if (SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            if (subtype == MFVideoFormat_NV12) pix_fmt_ = PixelFormat::NV12;
            else if (subtype == MFVideoFormat_I420) pix_fmt_ = PixelFormat::I420;
            else if (subtype == MFVideoFormat_YUY2) pix_fmt_ = PixelFormat::YUY2;
            else pix_fmt_ = PixelFormat::RGB32;
        }
        LOG_INFO("Video reconfigured: %dx%d @ %.1f fps (fmt=%d)",
                 w, h, vi_.fps, (int)pix_fmt_);
    }
}

bool MediaSource::OpenHls(const wchar_t* url) {
    hls_ = new (std::nothrow) HlsManager();
    if (!hls_) {
        LOG_ERROR("Out of memory: HlsManager");
        return false;
    }
    if (!hls_->Open(url)) {
        delete hls_; hls_ = nullptr;
        return false;
    }
    is_live_ = hls_->IsLive();
    duration_ = hls_->Duration();
    LOG_INFO("HLS: %s, duration: %.1f s", is_live_ ? "LIVE" : "VOD", duration_);
    return true;
}

void MediaSource::Close() {
    if (hls_) hls_->Close();
    reader_.reset();
    if (hls_) { delete hls_; hls_ = nullptr; }
    is_live_ = false;
    has_video_ = false;
    has_audio_ = false;
    video_stream_ = (DWORD)-1;
    audio_stream_ = (DWORD)-1;
    duration_ = 0;
    last_position_ = 0;
    pix_fmt_ = PixelFormat::Unknown;
}

bool MediaSource::HasNewHlsData() {
    if (!hls_) return false;
    return hls_->GetByteStream() && hls_->GetByteStream()->CheckAndClearNeedsWake();
}

bool MediaSource::HlsByteStreamHasData() {
    return hls_ && hls_->GetByteStream() && hls_->GetByteStream()->HasUnreadData();
}

bool MediaSource::Seek(double seconds) {
    if (!reader_) return false;
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(seconds * 10000000.0);
    HRESULT hr = reader_->SetCurrentPosition(GUID_NULL_VAL, var);
    PropVariantClear(&var);
    if (SUCCEEDED(hr)) last_position_ = seconds;
    return SUCCEEDED(hr);
}

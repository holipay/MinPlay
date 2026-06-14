#define COBJMACROS
#define INITGUID
#include "media_source.h"
#include "../util/log.h"
#include <windows.h>
#include <initguid.h>
#include <guiddef.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

static const GUID _GUID_NULL_VAL = {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}};
#define GUID_NULL_REF &_GUID_NULL_VAL

struct MediaSource {
    IMFSourceReader* reader;
    int has_video;
    int has_audio;
    DWORD video_stream;
    DWORD audio_stream;
    VideoInfo vi;
    AudioInfo ai;
    double duration;
    double last_position;
    PixelFormat pix_fmt;
};

static HRESULT get_uint64_pair(IMFAttributes* attr, REFGUID key,
                               UINT32* a, UINT32* b) {
    UINT64 val = 0;
    HRESULT hr = IMFAttributes_GetUINT64(attr, key, &val);
    if (SUCCEEDED(hr)) {
        *a = (UINT32)(val >> 32);
        *b = (UINT32)(val & 0xFFFFFFFF);
    }
    return hr;
}

MediaSource* media_open(const wchar_t* url) {
    return media_open_with_callback(url, NULL);
}

MediaSource* media_open_with_callback(const wchar_t* url,
                                       IMFSourceReaderCallback* cb) {
    MediaSource* src = (MediaSource*)calloc(1, sizeof(MediaSource));
    if (!src) return NULL;

    IMFAttributes* sattrs = NULL;
    MFCreateAttributes(&sattrs, cb ? 3 : 2);
    IMFAttributes_SetUINT32(sattrs, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    IMFAttributes_SetUINT32(sattrs, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (cb) {
        IMFAttributes_SetUnknown(sattrs, &MF_SOURCE_READER_ASYNC_CALLBACK, (IUnknown*)cb);
    }

    HRESULT hr = MFCreateSourceReaderFromURL(url, sattrs, &src->reader);
    if (sattrs) sattrs->lpVtbl->Release(sattrs);
    if (FAILED(hr)) {
        LOG_ERROR("MFCreateSourceReaderFromURL failed: 0x%08lX", hr);
        free(src);
        return NULL;
    }

    IMFMediaType* vmt = NULL;
    MFCreateMediaType(&vmt);
    IMFMediaType_SetGUID(vmt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);

    struct { const GUID* fmt; enum PixelFormat pf; } fmts[] = {
        { &MFVideoFormat_ARGB32, PF_RGB32 },
        { &MFVideoFormat_RGB32,  PF_RGB32 },
        { &MFVideoFormat_NV12,   PF_NV12  },
        { &MFVideoFormat_YUY2,   PF_YUY2  },
        { &MFVideoFormat_I420,   PF_I420  },
    };
    for (int fi = 0; fi < 5; fi++) {
        IMFMediaType_SetGUID(vmt, &MF_MT_SUBTYPE, fmts[fi].fmt);
        hr = IMFSourceReader_SetCurrentMediaType(
            src->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, vmt);
        if (SUCCEEDED(hr)) {
            src->has_video = 1;
            src->pix_fmt = fmts[fi].pf;
            IMFMediaType* native = NULL;
            IMFSourceReader_GetCurrentMediaType(
                src->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, &native);
            UINT32 w = 0, h = 0, num = 0, den = 0;
            get_uint64_pair((IMFAttributes*)native, &MF_MT_FRAME_SIZE, &w, &h);
            get_uint64_pair((IMFAttributes*)native, &MF_MT_FRAME_RATE, &num, &den);
            src->vi.width = (int)w;
            src->vi.height = (int)h;
            src->vi.fps = den ? (double)num / den : 30.0;
            IMFMediaType_Release(native);
            LOG_INFO("Video: %dx%d @ %.1f fps (fmt=%d)", w, h, src->vi.fps, src->pix_fmt);
            break;
        }
    }
    IMFMediaType_Release(vmt);

    IMFSourceReader_SetStreamSelection(
        src->reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    IMFMediaType* amt = NULL;
    MFCreateMediaType(&amt);
    IMFMediaType_SetGUID(amt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(amt, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(amt, &MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
    IMFMediaType_SetUINT32(amt, &MF_MT_AUDIO_NUM_CHANNELS, 2);
    IMFMediaType_SetUINT32(amt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    hr = IMFSourceReader_SetCurrentMediaType(
        src->reader, MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, amt);
    if (SUCCEEDED(hr)) {
        src->has_audio = 1;
        src->ai.sample_rate = 44100;
        src->ai.channels = 2;
        src->ai.bits_per_sample = 16;
        LOG_INFO("Audio: 44100 Hz, 2 ch, 16 bit");
    }
    IMFMediaType_Release(amt);

    PROPVARIANT dur;
    PropVariantInit(&dur);
    hr = IMFSourceReader_GetPresentationAttribute(
        src->reader, MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &dur);
    if (SUCCEEDED(hr) && (dur.vt == VT_I8 || dur.vt == VT_UI8))
        src->duration = dur.hVal.QuadPart / 10000000.0;
    PropVariantClear(&dur);
    LOG_INFO("Duration: %.1f s", src->duration);

    src->video_stream = (DWORD)-1;
    src->audio_stream = (DWORD)-1;
    for (DWORD i = 0; i < 16; i++) {
        IMFMediaType* stmtype = NULL;
        hr = IMFSourceReader_GetNativeMediaType(src->reader, i, 0, &stmtype);
        if (FAILED(hr)) break;
        GUID maj = {0};
        IMFMediaType_GetGUID(stmtype, &MF_MT_MAJOR_TYPE, &maj);
        if (IsEqualGUID(&maj, &MFMediaType_Video) && src->video_stream == (DWORD)-1) {
            src->video_stream = i;
        } else if (IsEqualGUID(&maj, &MFMediaType_Audio) && src->audio_stream == (DWORD)-1) {
            src->audio_stream = i;
        }
        IMFMediaType_Release(stmtype);
        if (src->video_stream != (DWORD)-1 && src->audio_stream != (DWORD)-1) break;
    }
    LOG_INFO("Streams: video=%lu audio=%lu", src->video_stream, src->audio_stream);

    return src;
}

void media_close(MediaSource* src) {
    if (!src) return;
    if (src->reader) IMFSourceReader_Release(src->reader);
    free(src);
}

int media_read_video(MediaSource* src, MediaFrame* frame) {
    if (!src || !src->reader || !frame) return -1;

    IMFSample* sample = NULL;
    DWORD flags = 0, stream_index = 0;
    LONGLONG timestamp = 0;

    HRESULT hr = IMFSourceReader_ReadSample(
        src->reader, MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
        &stream_index, &flags, &timestamp, &sample);
    if (FAILED(hr)) return -1;
    if (flags & MF_SOURCE_READERF_ERROR) return -1;
    if (!sample) return 0;
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) return 0;

    frame->timestamp = timestamp / 10000000.0;
    frame->owns_data = 1;

    IMFMediaBuffer* buf = NULL;
    IMFSample_ConvertToContiguousBuffer(sample, &buf);
    BYTE* data = NULL;
    DWORD max_len = 0, cur_len = 0;
    IMFMediaBuffer_Lock(buf, &data, &max_len, &cur_len);

    frame->type = 1;
    frame->size = (int)cur_len;
    frame->data = (uint8_t*)malloc(cur_len);
    memcpy(frame->data, data, cur_len);

    UINT32 w = 0, h = 0;
    IMFMediaType* mt = NULL;
    IMFSourceReader_GetCurrentMediaType(src->reader, stream_index, &mt);
    if (mt) {
        get_uint64_pair((IMFAttributes*)mt, &MF_MT_FRAME_SIZE, &w, &h);
        IMFMediaType_Release(mt);
    }
    frame->width = (int)w;
    frame->height = (int)h;
    frame->stride = (int)(w * 4);
    src->last_position = frame->timestamp;

    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(sample);
    return 1;
}

int media_read_audio(MediaSource* src, MediaFrame* frame) {
    if (!src || !src->reader || !frame) return -1;

    IMFSample* sample = NULL;
    DWORD flags = 0, stream_index = 0;
    LONGLONG timestamp = 0;

    HRESULT hr = IMFSourceReader_ReadSample(
        src->reader, src->audio_stream, 0,
        &stream_index, &flags, &timestamp, &sample);
    if (FAILED(hr)) return -1;
    if (flags & MF_SOURCE_READERF_ERROR) return -1;
    if (!sample) return 0;
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) return 0;

    frame->timestamp = timestamp / 10000000.0;
    frame->owns_data = 1;

    IMFMediaBuffer* buf = NULL;
    IMFSample_ConvertToContiguousBuffer(sample, &buf);
    BYTE* data = NULL;
    DWORD max_len = 0, cur_len = 0;
    IMFMediaBuffer_Lock(buf, &data, &max_len, &cur_len);

    frame->type = 0;
    frame->size = (int)cur_len;
    frame->data = (uint8_t*)malloc(cur_len);
    memcpy(frame->data, data, cur_len);
    frame->sample_rate = src->ai.sample_rate;
    frame->channels = src->ai.channels;
    frame->bits_per_sample = src->ai.bits_per_sample;
    src->last_position = frame->timestamp;

    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(sample);
    return 1;
}

int media_read(MediaSource* src, MediaFrame* frame) {
    if (!src || !src->reader || !frame) return -1;

    IMFSample* sample = NULL;
    DWORD flags = 0, stream_index = 0;
    LONGLONG timestamp = 0;

    HRESULT hr = IMFSourceReader_ReadSample(
        src->reader, MF_SOURCE_READER_ANY_STREAM, 0,
        &stream_index, &flags, &timestamp, &sample);
    if (FAILED(hr)) return -1;
    if (flags & MF_SOURCE_READERF_ERROR) return -1;
    if (!sample) return 0;
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) return 0;

    frame->timestamp = timestamp / 10000000.0;
    frame->owns_data = 1;

    IMFMediaBuffer* buf = NULL;
    IMFSample_ConvertToContiguousBuffer(sample, &buf);
    BYTE* data = NULL;
    DWORD max_len = 0, cur_len = 0;
    IMFMediaBuffer_Lock(buf, &data, &max_len, &cur_len);

    GUID major_type = {0};
    IMFMediaType* mt = NULL;
    IMFSourceReader_GetCurrentMediaType(src->reader, stream_index, &mt);
    if (mt) {
        IMFMediaType_GetGUID(mt, &MF_MT_MAJOR_TYPE, &major_type);
        IMFMediaType_Release(mt);
    }

    if (IsEqualGUID(&major_type, &MFMediaType_Video)) {
        frame->type = 1;
        frame->size = (int)cur_len;
        frame->data = (uint8_t*)malloc(cur_len);
        memcpy(frame->data, data, cur_len);
        src->last_position = frame->timestamp;

        UINT32 w = 0, h = 0;
        IMFMediaType* vmt = NULL;
        IMFSourceReader_GetCurrentMediaType(src->reader, stream_index, &vmt);
        if (vmt) {
            get_uint64_pair((IMFAttributes*)vmt, &MF_MT_FRAME_SIZE, &w, &h);
            IMFMediaType_Release(vmt);
        }
        frame->width = (int)w;
        frame->height = (int)h;
        frame->stride = (int)(w * 4);
    } else {
        frame->type = 0;
        frame->size = (int)cur_len;
        frame->data = (uint8_t*)malloc(cur_len);
        memcpy(frame->data, data, cur_len);
        frame->sample_rate = src->ai.sample_rate;
        frame->channels = src->ai.channels;
        frame->bits_per_sample = src->ai.bits_per_sample;
        src->last_position = frame->timestamp;
    }

    IMFMediaBuffer_Unlock(buf);
    IMFMediaBuffer_Release(buf);
    IMFSample_Release(sample);
    return 1;
}

void media_free_frame(MediaFrame* frame) {
    if (frame && frame->data && frame->owns_data) {
        free(frame->data);
        frame->data = NULL;
    }
}

int media_seek(MediaSource* src, double seconds) {
    if (!src || !src->reader) return -1;
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(seconds * 10000000.0);
    HRESULT hr = IMFSourceReader_SetCurrentPosition(src->reader, GUID_NULL_REF, &var);
    PropVariantClear(&var);
    if (SUCCEEDED(hr)) src->last_position = seconds;
    return SUCCEEDED(hr) ? 0 : -1;
}

double media_get_duration(MediaSource* src) { return src ? src->duration : 0; }
double media_get_position(MediaSource* src) { return src ? src->last_position : 0; }
int media_has_video(MediaSource* src) { return src ? src->has_video : 0; }
int media_has_audio(MediaSource* src) { return src ? src->has_audio : 0; }
int media_get_video_info(MediaSource* src, VideoInfo* info) {
    if (!src || !info) return -1; *info = src->vi; return 0;
}
int media_get_audio_info(MediaSource* src, AudioInfo* info) {
    if (!src || !info) return -1; *info = src->ai; return 0;
}

PixelFormat media_get_pixel_format(MediaSource* src) {
    return src ? src->pix_fmt : PF_UNKNOWN;
}

IMFSourceReader* media_get_reader(MediaSource* src) {
    return src ? src->reader : NULL;
}

DWORD media_get_video_stream(MediaSource* src) {
    return src ? src->video_stream : (DWORD)-1;
}

DWORD media_get_audio_stream(MediaSource* src) {
    return src ? src->audio_stream : (DWORD)-1;
}

#define COBJMACROS
#define INITGUID
#include "source_reader_callback.h"
#include "player.h"
#include "../audio_out/audio_out.h"
#include "../util/log.h"
#include <stdlib.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

struct SourceReaderCallback {
    IMFSourceReaderCallbackVtbl* vtbl;
    volatile LONG ref_count;

    HWND            hwnd;
    IMFSourceReader* reader;
    DWORD           video_stream;
    DWORD           audio_stream;
    AudioOut*       ao;
    struct Player*  player;

    CRITICAL_SECTION lock;
    volatile int     running;
    volatile int     video_eof;
    volatile int     audio_eof;
    volatile LONG   video_pending;
};

static HRESULT STDMETHODCALLTYPE cb_QueryInterface(IMFSourceReaderCallback* This,
                                                    REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFSourceReaderCallback)) {
        *ppvObject = This;
        This->lpVtbl->AddRef(This);
        return S_OK;
    }
    *ppvObject = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cb_AddRef(IMFSourceReaderCallback* This) {
    SourceReaderCallback* cb = (SourceReaderCallback*)This;
    return InterlockedIncrement(&cb->ref_count);
}

static ULONG STDMETHODCALLTYPE cb_Release(IMFSourceReaderCallback* This) {
    SourceReaderCallback* cb = (SourceReaderCallback*)This;
    LONG count = InterlockedDecrement(&cb->ref_count);
    if (count == 0) {
        free(cb->vtbl);
        free(cb);
    }
    return (ULONG)count;
}

static HRESULT STDMETHODCALLTYPE cb_OnEvent(IMFSourceReaderCallback* This,
                                              DWORD dwStreamIndex,
                                              IMFMediaEvent* pEvent) {
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_OnReadSample(IMFSourceReaderCallback* This,
                                                   HRESULT hrStatus,
                                                   DWORD dwStreamIndex,
                                                   DWORD dwStreamFlags,
                                                   LONGLONG llTimestamp,
                                                   IMFSample* pSample) {
    SourceReaderCallback* cb = (SourceReaderCallback*)This;

    if (FAILED(hrStatus) || !cb->running) {
        if (pSample) IMFSample_Release(pSample);
        return S_OK;
    }

    if (dwStreamFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
        if (dwStreamIndex == cb->video_stream)
            cb->video_eof = 1;
        else if (dwStreamIndex == cb->audio_stream)
            cb->audio_eof = 1;
        if (pSample) IMFSample_Release(pSample);
        return S_OK;
    }

    if (pSample) {
        if (dwStreamIndex == cb->audio_stream && cb->ao) {
            IMFMediaBuffer* buf = NULL;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(pSample, &buf))) {
                BYTE* data = NULL;
                DWORD max_len = 0, cur_len = 0;
                IMFMediaBuffer_Lock(buf, &data, &max_len, &cur_len);
                ao_write(cb->ao, data, (int)cur_len);
                ao_set_pts(cb->ao, llTimestamp / 10000000.0);
                IMFMediaBuffer_Unlock(buf);
                IMFMediaBuffer_Release(buf);
            }
        } else if (dwStreamIndex == cb->video_stream && cb->player) {
            /* Process video directly in MF callback thread (same as audio) */
            player_process_video_frame(cb->player, pSample, llTimestamp);
        }
    }

    if (cb->running && cb->reader) {
        if (dwStreamIndex == cb->audio_stream && cb->ao) {
            if (ao_get_free(cb->ao) > 256 * 1024)
                IMFSourceReader_ReadSample(cb->reader, dwStreamIndex, 0, NULL, NULL, NULL, NULL);
        } else if (dwStreamIndex == cb->video_stream) {
            LONG vp = InterlockedIncrement(&cb->video_pending);
            if (vp < 2) {
                IMFSourceReader_ReadSample(cb->reader, dwStreamIndex, 0, NULL, NULL, NULL, NULL);
            }
        }
    }

    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cb_OnFlush(IMFSourceReaderCallback* This,
                                              DWORD dwStreamIndex) {
    return S_OK;
}

SourceReaderCallback* src_cb_create(HWND hwnd) {
    SourceReaderCallback* cb = (SourceReaderCallback*)calloc(1, sizeof(SourceReaderCallback));
    if (!cb) return NULL;

    cb->vtbl = (IMFSourceReaderCallbackVtbl*)calloc(1, sizeof(IMFSourceReaderCallbackVtbl));
    cb->vtbl->QueryInterface = cb_QueryInterface;
    cb->vtbl->AddRef = cb_AddRef;
    cb->vtbl->Release = cb_Release;
    cb->vtbl->OnEvent = cb_OnEvent;
    cb->vtbl->OnReadSample = cb_OnReadSample;
    cb->vtbl->OnFlush = cb_OnFlush;

    cb->ref_count = 1;
    cb->hwnd = hwnd;
    cb->running = 0;
    InitializeCriticalSection(&cb->lock);

    return cb;
}

void src_cb_destroy(SourceReaderCallback* cb) {
    if (!cb) return;
    cb->running = 0;
    DeleteCriticalSection(&cb->lock);
    if (cb->ref_count == 1)
        cb->vtbl->Release((IMFSourceReaderCallback*)cb);
}

void src_cb_set_audio_out(SourceReaderCallback* cb, AudioOut* ao) {
    if (cb) cb->ao = ao;
}

void src_cb_set_player(SourceReaderCallback* cb, struct Player* player) {
    if (cb) cb->player = player;
}

HRESULT src_cb_set_on_source_reader(SourceReaderCallback* cb,
                                     IMFSourceReader* reader,
                                     DWORD video_stream,
                                     DWORD audio_stream) {
    if (!cb || !reader) return E_INVALIDARG;
    cb->reader = reader;
    cb->video_stream = video_stream;
    cb->audio_stream = audio_stream;
    return S_OK;
}

HRESULT src_cb_start_reading(SourceReaderCallback* cb) {
    if (!cb || !cb->reader) return E_FAIL;

    EnterCriticalSection(&cb->lock);
    cb->running = 1;
    cb->video_eof = 0;
    cb->audio_eof = 0;
    LeaveCriticalSection(&cb->lock);

    HRESULT hr;
    if (cb->video_stream != (DWORD)-1) {
        hr = IMFSourceReader_ReadSample(cb->reader, cb->video_stream, 0,
                                         NULL, NULL, NULL, NULL);
        if (FAILED(hr)) LOG_WARN("ReadSample video failed: 0x%08lX", hr);
    }
    if (cb->audio_stream != (DWORD)-1) {
        hr = IMFSourceReader_ReadSample(cb->reader, cb->audio_stream, 0,
                                         NULL, NULL, NULL, NULL);
        if (FAILED(hr)) LOG_WARN("ReadSample audio failed: 0x%08lX", hr);
    }
    return S_OK;
}

HRESULT src_cb_request_video_read(SourceReaderCallback* cb) {
    if (!cb || !cb->reader) return E_FAIL;
    if (cb->video_stream == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&cb->lock);
    int ok = cb->running && !cb->video_eof;
    LeaveCriticalSection(&cb->lock);
    if (!ok) return E_FAIL;
    return IMFSourceReader_ReadSample(cb->reader, cb->video_stream, 0,
                                      NULL, NULL, NULL, NULL);
}

HRESULT src_cb_request_audio_read(SourceReaderCallback* cb) {
    if (!cb || !cb->reader) return E_FAIL;
    if (cb->audio_stream == (DWORD)-1) return E_FAIL;
    EnterCriticalSection(&cb->lock);
    int ok = cb->running && !cb->audio_eof;
    LeaveCriticalSection(&cb->lock);
    if (!ok) return E_FAIL;
    return IMFSourceReader_ReadSample(cb->reader, cb->audio_stream, 0,
                                      NULL, NULL, NULL, NULL);
}

HRESULT src_cb_stop(SourceReaderCallback* cb) {
    if (!cb) return E_FAIL;
    EnterCriticalSection(&cb->lock);
    cb->running = 0;
    LeaveCriticalSection(&cb->lock);
    return S_OK;
}

int src_cb_is_video_eof(SourceReaderCallback* cb) {
    return cb ? cb->video_eof : 1;
}

int src_cb_is_audio_eof(SourceReaderCallback* cb) {
    return cb ? cb->audio_eof : 1;
}

void src_cb_consume_video(SourceReaderCallback* cb) {
    if (cb) InterlockedDecrement(&cb->video_pending);
}




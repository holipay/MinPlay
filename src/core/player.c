#define COBJMACROS
#include "player.h"
#include "source_reader_callback.h"
#include "../media/media_source.h"
#include "../video_out/video_out.h"
#include "../audio_out/audio_out.h"
#include "../util/log.h"
#include <stdlib.h>
#include <string.h>
#include <mfapi.h>
#include <mfidl.h>

#pragma comment(lib, "ole32.lib")

#define VQ_SIZE 32

typedef struct {
    uint8_t* data;
    int      size;
    double   timestamp;
} VFrame;

struct Player {
    HWND            hwnd;
    MediaSource*    source;
    VideoOut*       vo;
    AudioOut*       ao;

    PlayerState     state;
    int             has_video;
    int             has_audio;

    SourceReaderCallback* callback;

    CRITICAL_SECTION frame_lock;
    uint8_t*        frame_buf;
    int             frame_buf_size;
    volatile int    frame_ready;
    int             frame_w;
    int             frame_h;

    LARGE_INTEGER   perf_freq;
    double          start_time;
    volatile double pause_offset;
    volatile double pause_start;

    VFrame          vq[VQ_SIZE];
    volatile int    vq_head;
    volatile int    vq_tail;
    CRITICAL_SECTION vq_lock;
    HANDLE          vq_event;
    HANDLE          vthread;
    volatile int    vthread_running;
    double          paint_frame_ts;
    volatile int    vtime_initialized;
    double          audio_clock_base;
};

static double get_time_sec(Player* p) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (double)li.QuadPart / p->perf_freq.QuadPart;
}

static double elapsed_sec(Player* p) {
    return get_time_sec(p) - p->start_time - p->pause_offset;
}

static DWORD WINAPI video_thread_proc(LPVOID arg) {
    Player* p = (Player*)arg;
    double local_ts = 0;
    int frame_count = 0;
    int drop_count = 0;

    while (p->vthread_running) {
        WaitForSingleObject(p->vq_event, 100);
        if (!p->vthread_running) break;
        if (p->state != STATE_PLAYING) continue;

        for (;;) {
            EnterCriticalSection(&p->vq_lock);
            int head = p->vq_head;
            int tail = p->vq_tail;
            if (head == tail) {
                LeaveCriticalSection(&p->vq_lock);
                ResetEvent(p->vq_event);
                break;
            }
            VFrame* f = &p->vq[head];
            local_ts = f->timestamp;
            int size = f->size;

            int next_head = (head + 1) % VQ_SIZE;
            if (next_head != tail) {
                free(f->data);
                f->data = NULL;
                p->vq_head = next_head;
                drop_count++;
                LeaveCriticalSection(&p->vq_lock);
                continue;
            }

            EnterCriticalSection(&p->frame_lock);
            if (p->frame_buf && size <= p->frame_buf_size) {
                memcpy(p->frame_buf, f->data, size);
                p->frame_ready = 1;
                p->paint_frame_ts = local_ts;

                IMFMediaType* mt = NULL;
                IMFSourceReader_GetCurrentMediaType(media_get_reader(p->source),
                                                    media_get_video_stream(p->source), &mt);
                if (mt) {
                    UINT64 size_val = 0;
                    IMFAttributes_GetUINT64((IMFAttributes*)mt, &MF_MT_FRAME_SIZE, &size_val);
                    p->frame_w = (int)(size_val >> 32);
                    p->frame_h = (int)(size_val & 0xFFFFFFFF);
                    IMFMediaType_Release(mt);
                }
            }
            LeaveCriticalSection(&p->frame_lock);

            free(f->data);
            f->data = NULL;
            p->vq_head = next_head;
            LeaveCriticalSection(&p->vq_lock);

            InvalidateRect(p->hwnd, NULL, FALSE);
            frame_count++;
        }
        if (!src_cb_is_video_eof(p->callback)) {
            src_cb_request_video_read(p->callback);
        }
    }
    return 0;
}

Player* player_create(HWND hwnd) {
    Player* p = (Player*)calloc(1, sizeof(Player));
    p->hwnd = hwnd;
    p->state = STATE_STOPPED;
    InitializeCriticalSection(&p->frame_lock);
    InitializeCriticalSection(&p->vq_lock);
    p->vq_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    QueryPerformanceFrequency(&p->perf_freq);
    p->frame_buf_size = 3840 * 2160 * 4;
    p->frame_buf = malloc(p->frame_buf_size);
    return p;
}

int player_open(Player* p, const wchar_t* url) {
    if (!p) return 0;

    p->callback = src_cb_create(p->hwnd);
    if (!p->callback) {
        LOG_ERROR("Failed to create source reader callback");
        return 0;
    }

    p->source = media_open_with_callback(url, (IMFSourceReaderCallback*)p->callback);
    if (!p->source) {
        LOG_ERROR("Failed to open media source");
        src_cb_destroy(p->callback);
        p->callback = NULL;
        return 0;
    }

    p->has_video = media_has_video(p->source);
    p->has_audio = media_has_audio(p->source);

    if (p->has_audio) {
        AudioInfo ai;
        media_get_audio_info(p->source, &ai);
        p->ao = ao_create(ai.sample_rate, ai.channels, ai.bits_per_sample);
    }
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    p->vo = vo_create(p->hwnd, rc.right, rc.bottom);

    src_cb_set_on_source_reader(p->callback, media_get_reader(p->source),
                                 media_get_video_stream(p->source),
                                 media_get_audio_stream(p->source));
    return 1;
}

void player_close(Player* p) {
    if (!p) return;
    player_stop(p);

    if (p->vthread) {
        p->vthread_running = 0;
        SetEvent(p->vq_event);
        WaitForSingleObject(p->vthread, 3000);
        CloseHandle(p->vthread);
        p->vthread = NULL;
    }

    EnterCriticalSection(&p->vq_lock);
    for (int i = 0; i < VQ_SIZE; i++) {
        if (p->vq[i].data) { free(p->vq[i].data); p->vq[i].data = NULL; }
    }
    p->vq_head = 0;
    p->vq_tail = 0;
    LeaveCriticalSection(&p->vq_lock);

    if (p->vo) { vo_destroy(p->vo); p->vo = NULL; }
    if (p->ao) { ao_destroy(p->ao); p->ao = NULL; }
    if (p->source) { media_close(p->source); p->source = NULL; }
    if (p->callback) { src_cb_destroy(p->callback); p->callback = NULL; }
}

void player_destroy(Player* p) {
    if (!p) return;
    if (p->frame_buf) { free(p->frame_buf); p->frame_buf = NULL; }
    DeleteCriticalSection(&p->frame_lock);
    DeleteCriticalSection(&p->vq_lock);
    if (p->vq_event) { CloseHandle(p->vq_event); p->vq_event = NULL; }
    free(p);
}

void player_play(Player* p) {
    if (!p || !p->source) return;
    p->state = STATE_PLAYING;
    p->start_time = get_time_sec(p);
    p->pause_offset = 0;
    p->audio_clock_base = 0;

    if (p->has_video && !p->vthread) {
        p->vthread_running = 1;
        p->vthread = CreateThread(NULL, 0, video_thread_proc, p, 0, NULL);
    }

    ao_resume(p->ao);
    src_cb_start_reading(p->callback);
}

void player_pause_toggle(Player* p) {
    if (!p) return;
    if (p->state == STATE_PLAYING) {
        p->state = STATE_PAUSED;
        p->pause_start = get_time_sec(p);
        SetEvent(p->vq_event);
        src_cb_stop(p->callback);
        ao_pause(p->ao);
    } else if (p->state == STATE_PAUSED) {
        p->state = STATE_PLAYING;
        p->pause_offset += get_time_sec(p) - p->pause_start;
        ao_resume(p->ao);
        src_cb_start_reading(p->callback);
    }
}

void player_stop(Player* p) {
    if (!p) return;
    p->state = STATE_STOPPED;
    SetEvent(p->vq_event);
    if (p->callback) src_cb_stop(p->callback);
    ao_reset(p->ao);
}

void player_seek(Player* p, double seconds) {
    if (!p || !p->source) return;
    if (seconds < 0) seconds = 0;
    int was_playing = (p->state == STATE_PLAYING);

    src_cb_stop(p->callback);
    p->audio_clock_base = seconds;
    ao_reset(p->ao);

    EnterCriticalSection(&p->vq_lock);
    for (int i = 0; i < VQ_SIZE; i++) {
        if (p->vq[i].data) { free(p->vq[i].data); p->vq[i].data = NULL; }
    }
    p->vq_head = 0;
    p->vq_tail = 0;
    LeaveCriticalSection(&p->vq_lock);

    media_seek(p->source, seconds);

    EnterCriticalSection(&p->frame_lock);
    p->frame_ready = 0;
    LeaveCriticalSection(&p->frame_lock);

    p->vtime_initialized = 0;

    if (was_playing) {
        p->state = STATE_PLAYING;
        p->start_time = get_time_sec(p) - seconds;
        p->pause_offset = 0;
        ao_resume(p->ao);
        src_cb_start_reading(p->callback);
    }
}

PlayerState player_get_state(Player* p) { return p ? p->state : STATE_STOPPED; }
double player_get_duration(Player* p) { return p && p->source ? media_get_duration(p->source) : 0; }
double player_get_position(Player* p) {
    if (!p) return 0;
    double pos = elapsed_sec(p);
    double dur = player_get_duration(p);
    if (pos > dur) pos = dur;
    return pos < 0 ? 0 : pos;
}
int player_has_video(Player* p) { return p ? p->has_video : 0; }
int player_has_audio(Player* p) { return p ? p->has_audio : 0; }

void player_process_video_frame(Player* p, IMFSample* sample, LONGLONG timestamp) {
    if (!p || !sample) return;
    if (p->state != STATE_PLAYING) {
        IMFSample_Release(sample);
        return;
    }

    IMFMediaBuffer* buf = NULL;
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) {
        BYTE* data = NULL;
        DWORD max_len = 0, cur_len = 0;
        IMFMediaBuffer_Lock(buf, &data, &max_len, &cur_len);

        EnterCriticalSection(&p->vq_lock);
        int next_tail = (p->vq_tail + 1) % VQ_SIZE;
        if (next_tail == p->vq_head) {
            int old_head = p->vq_head;
            free(p->vq[old_head].data);
            p->vq[old_head].data = NULL;
            p->vq_head = (old_head + 1) % VQ_SIZE;
            p->vtime_initialized = 0;
        }
        VFrame* f = &p->vq[p->vq_tail];
        f->data = (uint8_t*)malloc(cur_len);
        memcpy(f->data, data, cur_len);
        f->size = (int)cur_len;
        f->timestamp = timestamp / 10000000.0;
        p->vq_tail = next_tail;
        LeaveCriticalSection(&p->vq_lock);

        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_Release(buf);

        SetEvent(p->vq_event);
    }

    IMFSample_Release(sample);
}

void player_process_audio_frame(Player* p, IMFSample* sample, LONGLONG timestamp) {
    if (!p || !sample) return;

    int buffered = ao_get_buffered(p->ao);
    int bps = 44100 * 2 * 2;
    int max_buf = bps / 2;

    if (buffered < max_buf) {
        IMFMediaBuffer* buf = NULL;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) {
            BYTE* data = NULL;
            DWORD max_len = 0, cur_len = 0;
            IMFMediaBuffer_Lock(buf, &data, &max_len, &cur_len);
            ao_write(p->ao, data, (int)cur_len);
            ao_set_pts(p->ao, timestamp / 10000000.0);
            IMFMediaBuffer_Unlock(buf);
            IMFMediaBuffer_Release(buf);
        }
    }

    IMFSample_Release(sample);

    if (buffered < max_buf) {
        src_cb_request_audio_read(p->callback);
    }
}

void player_check_audio(Player* p) {
    if (!p || p->state != STATE_PLAYING || !p->has_audio) return;
    if (src_cb_is_audio_eof(p->callback)) return;
    if (src_cb_is_video_eof(p->callback)) {
        src_cb_request_audio_read(p->callback);
        return;
    }
    int buffered = ao_get_buffered(p->ao);
    int bps = 44100 * 2 * 2;
    int low_threshold = bps / 10;
    if (buffered < low_threshold) {
        src_cb_request_audio_read(p->callback);
    }
}

int player_is_audio_done(Player* p) {
    if (!p || !p->ao) return 1;
    return ao_get_buffered(p->ao) == 0 && !ao_is_playing(p->ao);
}

int player_is_finished(Player* p) {
    if (!p) return 1;
    int ve = src_cb_is_video_eof(p->callback);
    int ae = src_cb_is_audio_eof(p->callback);
    if (!ae) {
        ae = player_is_audio_done(p);
    }
    return ve && ae;
}

void player_paint(Player* p, HDC hdc, int w, int h) {
    if (!p) {
        RECT rc = {0, 0, w, h};
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return;
    }
    EnterCriticalSection(&p->frame_lock);
    if (p->frame_ready && p->frame_buf && p->frame_w > 0 && p->frame_h > 0) {
        HDC memdc = CreateCompatibleDC(hdc);
        HBITMAP hbm = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldbm = (HBITMAP)SelectObject(memdc, hbm);

        RECT rc_bg = {0, 0, w, h};
        FillRect(memdc, &rc_bg, (HBRUSH)GetStockObject(BLACK_BRUSH));

        BITMAPINFO bmi = {0};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = p->frame_w;
        bmi.bmiHeader.biHeight = -p->frame_h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        double sx = (double)w / p->frame_w;
        double sy = (double)h / p->frame_h;
        double s = sx < sy ? sx : sy;
        int dw = (int)(p->frame_w * s);
        int dh = (int)(p->frame_h * s);
        int dx = (w - dw) / 2;
        int dy = (h - dh) / 2;

        SetStretchBltMode(memdc, HALFTONE);
        SetBrushOrgEx(memdc, 0, 0, NULL);
        StretchDIBits(memdc, dx, dy, dw, dh,
            0, 0, p->frame_w, p->frame_h,
            p->frame_buf, &bmi, DIB_RGB_COLORS, SRCCOPY);

        BitBlt(hdc, 0, 0, w, h, memdc, 0, 0, SRCCOPY);

        SelectObject(memdc, oldbm);
        DeleteObject(hbm);
        DeleteDC(memdc);
    } else {
        RECT rc = {0, 0, w, h};
        FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    LeaveCriticalSection(&p->frame_lock);
}

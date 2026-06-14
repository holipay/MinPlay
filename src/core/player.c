#define COBJMACROS
#include "player.h"
#include "source_reader_callback.h"
#include "../media/media_source.h"
#include "../video_out/video_out.h"
#include "../audio_out/audio_out.h"
#include "../sync/sync.h"
#include "../util/log.h"
#include <stdlib.h>
#include <string.h>
#include <mfapi.h>
#include <mfidl.h>

#pragma comment(lib, "ole32.lib")

#define VQ_SIZE 32

typedef struct {
    uint8_t*    data;
    int         size;
    double      timestamp;
    PixelFormat pix_fmt;
} VFrame;

struct Player {
    HWND            hwnd;
    MediaSource*    source;
    VideoOut*       vo;
    AudioOut*       ao;

    PlayerState     state;
    int             has_video;
    int             has_audio;
    int             audio_bytes_per_sec;
    double          video_fps;

    SourceReaderCallback* callback;
    SyncContext*    sync;

    volatile LONG  video_first_frame_post;

    CRITICAL_SECTION frame_lock;
    uint8_t*        frame_buf;
    int             frame_buf_size;
    volatile int    frame_ready;
    int             frame_w;
    int             frame_h;
    int             frame_size;
    PixelFormat     pix_fmt;

    LARGE_INTEGER   perf_freq;
    double          start_time;
    volatile double pause_offset;
    volatile double pause_start;

    VFrame          vq[VQ_SIZE];
    volatile int    vq_head;
    volatile int    vq_tail;
    CRITICAL_SECTION vq_lock;

    int             win_w;
    int             win_h;
};

static double get_time_sec(Player* p) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (double)li.QuadPart / p->perf_freq.QuadPart;
}

static double elapsed_sec(Player* p) {
    return get_time_sec(p) - p->start_time - p->pause_offset;
}

void player_video_tick(Player* p) {
    if (!p || p->state != STATE_PLAYING) return;

    /* Keep video pipeline fed at display rate (instead of burst from callback) */
    if (p->has_video && p->callback && !src_cb_is_video_eof(p->callback)) {
        EnterCriticalSection(&p->vq_lock);
        int qsize = p->vq_tail - p->vq_head;
        if (qsize < 0) qsize += VQ_SIZE;
        LeaveCriticalSection(&p->vq_lock);
        if (qsize < 1)
            src_cb_request_video_read(p->callback);
    }

    /* Early-out: no frames in queue and no frame ready to render */
    if (p->vq_head == p->vq_tail && !p->frame_ready)
        return;

    /* Get master clock from ao PTS-based clock */
    double audio_clk = 0;
    if (p->has_audio && p->ao)
        audio_clk = ao_get_clock(p->ao);
    if (audio_clk <= 0.001)
        audio_clk = elapsed_sec(p);

    /* Find the best frame to display using sync decision */
    EnterCriticalSection(&p->vq_lock);
    for (;;) {
        int head = p->vq_head;
        int tail = p->vq_tail;
        if (head == tail) break;

        VFrame* f = &p->vq[head];
        SyncDecision sd = sync_video(p->sync, f->timestamp, audio_clk);
        int next = (head + 1) % VQ_SIZE;
        int has_more = (next != tail);

        if (sd.action == SYNC_DROP && has_more) {
            free(f->data); f->data = NULL;
            p->vq_head = next;
            src_cb_consume_video(p->callback);
            continue;
        }
        if (sd.action == SYNC_WAIT) {
            break;
        }
        /* SYNC_RENDER — display */
        int size = f->size;
        EnterCriticalSection(&p->frame_lock);
        if (p->frame_buf && size <= p->frame_buf_size) {
            memcpy(p->frame_buf, f->data, size);
            p->frame_ready = 1;
            p->frame_size = size;

            /* Update display size from current media type (may differ
               from negotiated type due to decoder alignment padding) */
            int old_w = p->frame_w, old_h = p->frame_h;
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
            if (p->frame_w == 0 || p->frame_h == 0) {
                p->frame_w = old_w;
                p->frame_h = old_h;
            }
        }
        LeaveCriticalSection(&p->frame_lock);

        free(f->data); f->data = NULL;
        p->vq_head = next;
        src_cb_consume_video(p->callback);
        break;
    }
    LeaveCriticalSection(&p->vq_lock);

    /* Refill pipeline after consumption */
    if (p->has_video && p->callback && !src_cb_is_video_eof(p->callback)) {
        EnterCriticalSection(&p->vq_lock);
        int qsize = p->vq_tail - p->vq_head;
        if (qsize < 0) qsize += VQ_SIZE;
        LeaveCriticalSection(&p->vq_lock);
        if (qsize < 1)
            src_cb_request_video_read(p->callback);
    }

    /* Render using cached window size */
    if (p->win_w > 0 && p->win_h > 0)
        player_render_d3d(p, p->win_w, p->win_h);
}

Player* player_create(HWND hwnd) {
    Player* p = (Player*)calloc(1, sizeof(Player));
    p->hwnd = hwnd;
    p->state = STATE_STOPPED;
    InitializeCriticalSection(&p->frame_lock);
    InitializeCriticalSection(&p->vq_lock);
    QueryPerformanceFrequency(&p->perf_freq);
    p->frame_buf_size = 3840 * 2160 * 4;
    p->frame_buf = malloc(p->frame_buf_size);
    RECT rc;
    GetClientRect(hwnd, &rc);
    p->win_w = rc.right;
    p->win_h = rc.bottom;
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
    p->pix_fmt = media_get_pixel_format(p->source);

    if (p->has_video) {
        VideoInfo vi;
        media_get_video_info(p->source, &vi);
        p->frame_w = vi.width;
        p->frame_h = vi.height;
        p->video_fps = vi.fps;
    }

    if (p->has_audio) {
        AudioInfo ai;
        media_get_audio_info(p->source, &ai);
        p->ao = ao_create(ai.sample_rate, ai.channels, ai.bits_per_sample);
        p->audio_bytes_per_sec = ai.sample_rate * ai.channels * (ai.bits_per_sample / 8);
        src_cb_set_audio_out(p->callback, p->ao);
    }

    /* Create sync context with adaptive window */
    double sync_window = 0.020;
    if (p->has_audio && p->ao && ao_is_exclusive(p->ao))
        sync_window = 0.010;
    p->sync = sync_create(sync_window);
    RECT rc;
    GetClientRect(p->hwnd, &rc);
    p->vo = vo_create(p->hwnd, rc.right, rc.bottom);

    src_cb_set_player(p->callback, p);
    src_cb_set_on_source_reader(p->callback, media_get_reader(p->source),
                                 media_get_video_stream(p->source),
                                 media_get_audio_stream(p->source));
    return 1;
}

void player_close(Player* p) {
    if (!p) return;
    player_stop(p);

    EnterCriticalSection(&p->vq_lock);
    for (int i = 0; i < VQ_SIZE; i++) {
        if (p->vq[i].data) { free(p->vq[i].data); p->vq[i].data = NULL; }
    }
    p->vq_head = 0;
    p->vq_tail = 0;
    LeaveCriticalSection(&p->vq_lock);

    if (p->vo) { vo_destroy(p->vo); p->vo = NULL; }
    if (p->ao) { ao_destroy(p->ao); p->ao = NULL; }
    if (p->sync) { sync_destroy(p->sync); p->sync = NULL; }
    if (p->source) { media_close(p->source); p->source = NULL; }
    if (p->callback) { src_cb_destroy(p->callback); p->callback = NULL; }
}

void player_destroy(Player* p) {
    if (!p) return;
    if (p->frame_buf) { free(p->frame_buf); p->frame_buf = NULL; }
    DeleteCriticalSection(&p->frame_lock);
    DeleteCriticalSection(&p->vq_lock);
    free(p);
}

void player_play(Player* p) {
    if (!p || !p->source) return;
    p->state = STATE_PLAYING;
    p->start_time = get_time_sec(p);
    p->pause_offset = 0;

    InterlockedExchange(&p->video_first_frame_post, 1);
    ao_resume(p->ao);
    src_cb_start_reading(p->callback);
}

void player_pause_toggle(Player* p) {
    if (!p) return;
    if (p->state == STATE_PLAYING) {
        p->state = STATE_PAUSED;
        p->pause_start = get_time_sec(p);
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
    if (p->callback) src_cb_stop(p->callback);
    ao_reset(p->ao);
}

void player_seek(Player* p, double seconds) {
    if (!p || !p->source) return;
    if (seconds < 0) seconds = 0;
    int was_playing = (p->state == STATE_PLAYING);

    src_cb_stop(p->callback);
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

    if (was_playing) {
        p->state = STATE_PLAYING;
        p->start_time = get_time_sec(p) - seconds;
        p->pause_offset = 0;
        InterlockedExchange(&p->video_first_frame_post, 1);
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

static uint8_t* convert_yuy2_to_nv12(const uint8_t* yuy2, int w, int h) {
    int y_size = w * h;
    int nv12_size = y_size + y_size / 2;
    uint8_t* nv12 = (uint8_t*)malloc(nv12_size);
    if (!nv12) return NULL;
    uint8_t* yp = nv12;
    uint8_t* uvp = nv12 + y_size;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col += 2) {
            int src_idx = (row * w + col) * 2;
            yp[row * w + col]     = yuy2[src_idx];
            yp[row * w + col + 1] = yuy2[src_idx + 2];
            if ((row & 1) == 0) {
                int uv_idx = (row / 2) * w + col;
                uvp[uv_idx]     = yuy2[src_idx + 1];
                uvp[uv_idx + 1] = yuy2[src_idx + 3];
            }
        }
    }
    return nv12;
}

static uint8_t* convert_i420_to_nv12(const uint8_t* i420, int w, int h) {
    int y_size = w * h;
    int nv12_size = y_size + y_size / 2;
    uint8_t* nv12 = (uint8_t*)malloc(nv12_size);
    if (!nv12) return NULL;
    memcpy(nv12, i420, y_size);
    uint8_t* uvp = nv12 + y_size;
    const uint8_t* up = i420 + y_size;
    const uint8_t* vp = up + (w / 2) * (h / 2);
    int uv_stride = w;
    for (int row = 0; row < h / 2; row++) {
        for (int col = 0; col < w / 2; col++) {
            uvp[row * uv_stride + col * 2]     = up[row * (w / 2) + col];
            uvp[row * uv_stride + col * 2 + 1] = vp[row * (w / 2) + col];
        }
    }
    return nv12;
}

void player_process_video_frame(Player* p, IMFSample* sample, LONGLONG timestamp) {
    if (!p || !sample) return;
    if (p->state != STATE_PLAYING)
        return;

    IMFMediaBuffer* buf = NULL;
    if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) {
        BYTE* data = NULL;
        DWORD max_len = 0, cur_len = 0;
        IMFMediaBuffer_Lock(buf, &data, &max_len, &cur_len);

        const uint8_t* frame_data = data;
        int frame_size = (int)cur_len;
        uint8_t* converted = NULL;
        PixelFormat vq_fmt = p->pix_fmt;

        if (vq_fmt == PF_RGB32 || vq_fmt == PF_NV12) {
            /* Pass through — GPU handles both natively */
        } else if (vq_fmt == PF_YUY2 && p->frame_w > 0 && p->frame_h > 0) {
            converted = convert_yuy2_to_nv12(data, p->frame_w, p->frame_h);
            if (converted) { frame_data = converted; vq_fmt = PF_NV12; frame_size = p->frame_w * p->frame_h * 3 / 2; }
        } else if (vq_fmt == PF_I420 && p->frame_w > 0 && p->frame_h > 0) {
            converted = convert_i420_to_nv12(data, p->frame_w, p->frame_h);
            if (converted) { frame_data = converted; vq_fmt = PF_NV12; frame_size = p->frame_w * p->frame_h * 3 / 2; }
        }

        EnterCriticalSection(&p->vq_lock);
        int was_empty = (p->vq_head == p->vq_tail);
        int next_tail = (p->vq_tail + 1) % VQ_SIZE;
        if (next_tail == p->vq_head) {
            int old_head = p->vq_head;
            free(p->vq[old_head].data);
            p->vq[old_head].data = NULL;
            p->vq_head = (old_head + 1) % VQ_SIZE;
        }
        VFrame* f = &p->vq[p->vq_tail];
        f->data = (uint8_t*)malloc(frame_size);
        memcpy(f->data, frame_data, frame_size);
        f->size = frame_size;
        f->timestamp = timestamp / 10000000.0;
        f->pix_fmt = vq_fmt;
        p->vq_tail = next_tail;
        LeaveCriticalSection(&p->vq_lock);

        free(converted);
        IMFMediaBuffer_Unlock(buf);
        IMFMediaBuffer_Release(buf);

        /* Wake main thread for first frame (then let the timer drive) */
        if (was_empty && InterlockedExchange(&p->video_first_frame_post, 0))
            PostMessage(p->hwnd, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
    }
}

void player_check_audio(Player* p) {
    if (!p || p->state != STATE_PLAYING) return;

    if (p->has_audio && !src_cb_is_audio_eof(p->callback)) {
        int buffered = ao_get_buffered(p->ao);
        int bps = p->audio_bytes_per_sec > 0 ? p->audio_bytes_per_sec : (44100 * 2 * 2);
        if (buffered < bps / 5) {
            src_cb_request_audio_read(p->callback);
        }
    }

    if (p->has_video && !src_cb_is_video_eof(p->callback)) {
        EnterCriticalSection(&p->vq_lock);
        int has_frames = (p->vq_head != p->vq_tail);
        LeaveCriticalSection(&p->vq_lock);
        if (!has_frames) {
            src_cb_request_video_read(p->callback);
        }
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

void player_resize(Player* p, int w, int h) {
    if (!p) return;
    p->win_w = w;
    p->win_h = h;
}

double player_get_video_fps(Player* p) {
    return p ? p->video_fps : 30.0;
}

void player_paint(Player* p, HDC hdc, int w, int h) {
    /* D3D11 handles rendering — WM_PAINT is a no-op */
    (void)p; (void)hdc; (void)w;
}

void player_render_d3d(Player* p, int w, int h) {
    if (!p || !p->vo) return;
    EnterCriticalSection(&p->frame_lock);
    if (p->frame_ready && p->frame_buf && p->frame_w > 0 && p->frame_h > 0) {
        vo_resize(p->vo, w, h);
        vo_render(p->vo, p->frame_buf, p->frame_w, p->frame_h, p->frame_size);
        p->frame_ready = 0;
    }
    LeaveCriticalSection(&p->frame_lock);
}

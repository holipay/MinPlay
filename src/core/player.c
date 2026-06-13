#include "player.h"
#include "../media/media_source.h"
#include "../video_out/video_out.h"
#include "../audio_out/audio_out.h"
#include "../util/log.h"
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")

typedef struct {
    double      audio_clock;
    double      video_clock;
    double      external_clock;
    CRITICAL_SECTION lock;
} Clock;

struct Player {
    HWND            hwnd;
    MediaSource*    source;
    VideoOut*       vo;
    AudioOut*       ao;

    PlayerState     state;
    volatile int    running;
    int             has_video;
    int             has_audio;

    Clock           clock;

    CRITICAL_SECTION frame_lock;
    uint8_t*        frame_buf;
    int             frame_buf_size;
    volatile int    frame_ready;
    int             frame_w;
    int             frame_h;

    HANDLE          decode_thread;
};

static void clock_init(Clock* c) {
    InitializeCriticalSection(&c->lock);
    c->audio_clock = 0;
    c->video_clock = 0;
    c->external_clock = 0;
}

static void clock_set_audio(Clock* c, double pts) {
    EnterCriticalSection(&c->lock);
    c->audio_clock = pts;
    LeaveCriticalSection(&c->lock);
}

static double clock_get_master(Clock* c, int has_audio) {
    EnterCriticalSection(&c->lock);
    double t = has_audio ? c->audio_clock : c->external_clock;
    LeaveCriticalSection(&c->lock);
    return t;
}

static int compute_video_delay(Clock* clk, double video_pts, int has_audio) {
    double master = clock_get_master(clk, has_audio);
    double diff = video_pts - master;
    if (diff < -0.1) return -1;
    else if (diff < -0.04) return 0;
    else if (diff > 0.5) return 200;
    else if (diff > 0.005) return (int)(diff * 1000);
    else return 0;
}

static DWORD WINAPI decode_thread_proc(LPVOID arg) {
    Player* p = (Player*)arg;
    MediaFrame frame = {0};

    while (p->running) {
        while (p->state == STATE_PAUSED && p->running)
            Sleep(10);
        if (!p->running) break;

        int ret = media_read(p->source, &frame);
        if (ret < 0) break;
        if (ret == 0) { Sleep(1); continue; }

        if (frame.type == 1) {
            EnterCriticalSection(&p->frame_lock);
            if (p->frame_buf && frame.size <= p->frame_buf_size) {
                memcpy(p->frame_buf, frame.data, frame.size);
                p->frame_ready = 1;
                p->frame_w = frame.width;
                p->frame_h = frame.height;
            }
            LeaveCriticalSection(&p->frame_lock);
            InvalidateRect(p->hwnd, NULL, FALSE);
        }
        else if (frame.type == 0) {
            ao_write(p->ao, frame.data, frame.size);
            clock_set_audio(&p->clock, frame.timestamp);
        }

        media_free_frame(&frame);
    }
    return 0;
}

Player* player_create(HWND hwnd) {
    Player* p = (Player*)calloc(1, sizeof(Player));
    p->hwnd = hwnd;
    p->state = STATE_STOPPED;
    p->running = 0;
    clock_init(&p->clock);
    InitializeCriticalSection(&p->frame_lock);
    p->frame_buf_size = 3840 * 2160 * 4;
    p->frame_buf = malloc(p->frame_buf_size);
    return p;
}

int player_open(Player* p, const wchar_t* url) {
    if (!p) return 0;
    p->source = media_open(url);
    if (!p->source) return 0;
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
    return 1;
}

void player_close(Player* p) {
    if (!p) return;
    player_stop(p);
    if (p->vo) { vo_destroy(p->vo); p->vo = NULL; }
    if (p->ao) { ao_destroy(p->ao); p->ao = NULL; }
    if (p->source) { media_close(p->source); p->source = NULL; }
}

void player_destroy(Player* p) {
    if (!p) return;
    if (p->frame_buf) { free(p->frame_buf); p->frame_buf = NULL; }
    DeleteCriticalSection(&p->frame_lock);
    DeleteCriticalSection(&p->clock.lock);
    free(p);
}

void player_play(Player* p) {
    if (!p || !p->source) return;
    p->state = STATE_PLAYING;
    p->running = 1;
    ao_resume(p->ao);
    p->decode_thread = CreateThread(NULL, 0, decode_thread_proc, p, 0, NULL);
}

void player_pause_toggle(Player* p) {
    if (!p) return;
    if (p->state == STATE_PLAYING) {
        p->state = STATE_PAUSED;
        ao_pause(p->ao);
    } else if (p->state == STATE_PAUSED) {
        p->state = STATE_PLAYING;
        ao_resume(p->ao);
    }
}

void player_stop(Player* p) {
    if (!p) return;
    p->running = 0;
    p->state = STATE_STOPPED;
    if (p->decode_thread) {
        WaitForSingleObject(p->decode_thread, 3000);
        CloseHandle(p->decode_thread);
        p->decode_thread = NULL;
    }
    ao_reset(p->ao);
}

void player_seek(Player* p, double seconds) {
    if (!p || !p->source) return;
    if (seconds < 0) seconds = 0;
    int was_playing = (p->state == STATE_PLAYING);
    p->running = 0;
    if (p->decode_thread) {
        WaitForSingleObject(p->decode_thread, 3000);
        CloseHandle(p->decode_thread);
        p->decode_thread = NULL;
    }
    ao_reset(p->ao);
    media_seek(p->source, seconds);
    EnterCriticalSection(&p->frame_lock);
    p->frame_ready = 0;
    LeaveCriticalSection(&p->frame_lock);
    clock_init(&p->clock);
    if (was_playing) {
        p->running = 1;
        p->state = STATE_PLAYING;
        ao_resume(p->ao);
        p->decode_thread = CreateThread(NULL, 0, decode_thread_proc, p, 0, NULL);
    }
}

PlayerState player_get_state(Player* p) { return p ? p->state : STATE_STOPPED; }
double player_get_duration(Player* p) { return p && p->source ? media_get_duration(p->source) : 0; }
double player_get_position(Player* p) { return p && p->source ? media_get_position(p->source) : 0; }
int player_has_video(Player* p) { return p ? p->has_video : 0; }
int player_has_audio(Player* p) { return p ? p->has_audio : 0; }

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

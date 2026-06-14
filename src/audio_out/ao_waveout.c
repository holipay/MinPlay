#include "audio_out.h"
#include <windows.h>
#include <stdio.h>

#pragma comment(lib, "winmm.lib")

#define WAVE_BUFS  16
#define WAVE_SIZE  16384
#define RING_SIZE  (1024 * 1024)

struct AudioOut {
    HWAVEOUT handle;
    WAVEHDR  headers[WAVE_BUFS];
    uint8_t  wave_bufs[WAVE_BUFS][WAVE_SIZE];
    volatile int write_idx;
    volatile int ready;

    uint8_t  ring[RING_SIZE];
    volatile int ring_head;
    volatile int ring_tail;
    CRITICAL_SECTION ring_lock;

    HANDLE    playback_thread;
    volatile int playing;

    int bytes_per_sec;
    volatile LONG64 hw_position;
    volatile LONG64 total_bytes_written;
    double last_write_pts;
    int last_write_size;
};

static DWORD WINAPI playback_thread_proc(LPVOID arg) {
    AudioOut* ao = (AudioOut*)arg;
    int wave_idx = 0;

    while (ao->playing) {
        MMTIME mt = {0};
        mt.wType = TIME_BYTES;
        if (waveOutGetPosition(ao->handle, &mt, sizeof(mt)) == MMSYSERR_NOERROR)
            ao->hw_position = (LONG64)mt.u.cb;

        if (!(ao->headers[wave_idx].dwFlags & WHDR_DONE)) {
            Sleep(1);
            continue;
        }

        int head = ao->ring_head;
        int tail = ao->ring_tail;
        int avail = (head - tail + RING_SIZE) % RING_SIZE;
        if (avail < WAVE_SIZE) {
            Sleep(1);
            continue;
        }

        memcpy(ao->wave_bufs[wave_idx], ao->ring + tail, WAVE_SIZE);
        ao->ring_tail = (tail + WAVE_SIZE) % RING_SIZE;

        ao->headers[wave_idx].dwBytesRecorded = 0;
        ao->headers[wave_idx].dwBufferLength = WAVE_SIZE;
        ao->headers[wave_idx].dwFlags = WHDR_PREPARED;
        ao->headers[wave_idx].dwLoops = 0;
        waveOutWrite(ao->handle, &ao->headers[wave_idx], sizeof(WAVEHDR));

        wave_idx = (wave_idx + 1) % WAVE_BUFS;
    }
    return 0;
}

AudioOut* ao_create(int rate, int channels, int bits) {
    AudioOut* ao = (AudioOut*)calloc(1, sizeof(AudioOut));
    InitializeCriticalSection(&ao->ring_lock);

    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = (WORD)channels;
    wfx.nSamplesPerSec  = (DWORD)rate;
    wfx.wBitsPerSample  = (WORD)bits;
    wfx.nBlockAlign     = (WORD)(channels * bits / 8);
    wfx.nAvgBytesPerSec = rate * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&ao->handle, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[WARN] waveOutOpen failed: %u\n", mr);
        ao->ready = 0;
        return ao;
    }

    ao->bytes_per_sec = wfx.nAvgBytesPerSec;

    for (int i = 0; i < WAVE_BUFS; i++) {
        memset(&ao->headers[i], 0, sizeof(WAVEHDR));
        ao->headers[i].lpData         = (LPSTR)ao->wave_bufs[i];
        ao->headers[i].dwBufferLength = WAVE_SIZE;
        waveOutPrepareHeader(ao->handle, &ao->headers[i], sizeof(WAVEHDR));
        ao->headers[i].dwFlags = WHDR_DONE;
    }

    ao->ready = 1;
    ao->playing = 1;
    ao->playback_thread = CreateThread(NULL, 0, playback_thread_proc, ao, 0, NULL);

    return ao;
}

void ao_write(AudioOut* ao, const uint8_t* data, int size) {
    if (!ao || !ao->ready) return;

    int head = ao->ring_head;
    int tail = ao->ring_tail;
    int avail = (tail - head - 1 + RING_SIZE) % RING_SIZE;

    int to_write = size < avail ? size : avail;
    int written = 0;

    while (to_write > 0) {
        int chunk = RING_SIZE - head;
        if (chunk > to_write) chunk = to_write;
        memcpy(ao->ring + head, data, chunk);
        head = (head + chunk) % RING_SIZE;
        data += chunk;
        to_write -= chunk;
        written += chunk;
    }
    ao->ring_head = head;
    ao->total_bytes_written += written;
    ao->last_write_size = written;
}

void ao_set_pts(AudioOut* ao, double pts) {
    if (!ao) return;
    ao->last_write_pts = pts;
}

double ao_get_clock(AudioOut* ao) {
    if (!ao || ao->bytes_per_sec <= 0) return 0;

    MMTIME mt = {0};
    mt.wType = TIME_BYTES;
    if (waveOutGetPosition(ao->handle, &mt, sizeof(mt)) != MMSYSERR_NOERROR)
        return ao->last_write_pts;
    LONG64 hw = (LONG64)mt.u.cb;

    double pts_at_head = ao->last_write_pts +
        (double)ao->last_write_size / ao->bytes_per_sec;
    LONG64 buffered = ao->total_bytes_written - hw;
    if (buffered < 0) buffered = 0;

    return pts_at_head - (double)buffered / ao->bytes_per_sec;
}

int ao_get_buffered(AudioOut* ao) {
    if (!ao) return 0;
    int head = ao->ring_head;
    int tail = ao->ring_tail;
    return (head - tail + RING_SIZE) % RING_SIZE;
}

int ao_get_free(AudioOut* ao) {
    if (!ao) return 0;
    int head = ao->ring_head;
    int tail = ao->ring_tail;
    return (tail - head - 1 + RING_SIZE) % RING_SIZE;
}

double ao_get_position_sec(AudioOut* ao) {
    if (!ao || ao->bytes_per_sec <= 0) return 0;
    return (double)ao->hw_position / ao->bytes_per_sec;
}

int ao_is_playing(AudioOut* ao) {
    if (!ao || !ao->ready) return 0;
    for (int i = 0; i < WAVE_BUFS; i++) {
        if (!(ao->headers[i].dwFlags & WHDR_DONE))
            return 1;
    }
    return 0;
}

void ao_pause(AudioOut* ao) {
    if (ao && ao->ready) waveOutPause(ao->handle);
}

void ao_resume(AudioOut* ao) {
    if (ao && ao->ready) waveOutRestart(ao->handle);
}

void ao_reset(AudioOut* ao) {
    if (!ao || !ao->ready) return;
    waveOutReset(ao->handle);
    ao->ring_head = 0;
    ao->ring_tail = 0;
    ao->hw_position = 0;
    ao->total_bytes_written = 0;
    ao->last_write_pts = 0;
    ao->last_write_size = 0;
    for (int i = 0; i < WAVE_BUFS; i++) {
        ao->headers[i].dwFlags = WHDR_DONE;
        ao->headers[i].dwBufferLength = WAVE_SIZE;
    }
}

void ao_destroy(AudioOut* ao) {
    if (!ao) return;
    ao->playing = 0;
    if (ao->playback_thread) {
        WaitForSingleObject(ao->playback_thread, 3000);
        CloseHandle(ao->playback_thread);
    }
    if (ao->ready) {
        waveOutReset(ao->handle);
        for (int i = 0; i < WAVE_BUFS; i++)
            waveOutUnprepareHeader(ao->handle, &ao->headers[i], sizeof(WAVEHDR));
        waveOutClose(ao->handle);
    }
    DeleteCriticalSection(&ao->ring_lock);
    free(ao);
}

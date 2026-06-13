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
};

static DWORD WINAPI playback_thread_proc(LPVOID arg) {
    AudioOut* ao = (AudioOut*)arg;
    int wave_idx = 0;

    while (ao->playing) {
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
    int free = (tail - head - 1 + RING_SIZE) % RING_SIZE;

    if (size > free) {
        int skip = size - free;
        ao->ring_tail = (tail + skip) % RING_SIZE;
    }

    while (size > 0) {
        int chunk = RING_SIZE - head;
        if (chunk > size) chunk = size;
        memcpy(ao->ring + head, data, chunk);
        head = (head + chunk) % RING_SIZE;
        data += chunk;
        size -= chunk;
    }
    ao->ring_head = head;
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

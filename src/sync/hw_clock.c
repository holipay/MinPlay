#include "hw_clock.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <stdlib.h>
#include <string.h>

static const IID _IID_IAudioClient_c = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const IID _IID_IAudioRenderClient_c = {0xF294ACFC,0x3146,0x4483,{0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2}};
static const IID _IID_IAudioClock_c = {0xCD63314F,0x3FBA,0x4A1B,{0x81,0x2C,0xEF,0x96,0x35,0x87,0x28,0xE7}};
static const IID _IID_IMMDeviceEnumerator_c = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID _CLSID_MMDeviceEnumerator_c = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};

struct HWClock {
    IAudioClient*       client;
    IAudioClock*        clock;
    IAudioRenderClient* renderer;
    UINT32              buffer_frames;
    UINT64              device_freq;
    double              latency;
    int                 channels;
    int                 bits;
    int                 frame_bytes;
    double              pts_base;
    double              device_base;
    CRITICAL_SECTION    lock;
    volatile int        running;
};

HWClock* hwclock_create(int sample_rate, int channels, int bits, int exclusive) {
    HWClock* clk = (HWClock*)calloc(1, sizeof(HWClock));
    if (!clk) return NULL;
    clk->channels = channels;
    clk->bits = bits;
    clk->frame_bytes = channels * bits / 8;
    InitializeCriticalSection(&clk->lock);

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice* device = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&_CLSID_MMDeviceEnumerator_c, NULL, CLSCTX_ALL,
                          &_IID_IMMDeviceEnumerator_c, (void**)&enumerator);
    if (FAILED(hr)) goto fail;

    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) goto fail;

    hr = device->lpVtbl->Activate(device, &_IID_IAudioClient_c, CLSCTX_ALL, NULL, (void**)&clk->client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) goto fail;

    WAVEFORMATEX* fmt = NULL;
    hr = clk->client->lpVtbl->GetMixFormat(clk->client, &fmt);
    if (FAILED(hr)) goto fail;

    REFERENCE_TIME buffer_dur = exclusive ? 30000 : 100000;
    DWORD flags = exclusive ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;

    hr = clk->client->lpVtbl->Initialize(clk->client,
        exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
        flags, buffer_dur, 0, fmt, NULL);
    if (FAILED(hr)) { CoTaskMemFree(fmt); goto fail; }

    clk->client->lpVtbl->GetBufferSize(clk->client, &clk->buffer_frames);
    clk->device_freq = fmt->nSamplesPerSec;

    REFERENCE_TIME sl = 0;
    clk->client->lpVtbl->GetStreamLatency(clk->client, &sl);
    clk->latency = sl / 10000000.0;
    CoTaskMemFree(fmt);

    hr = clk->client->lpVtbl->GetService(clk->client, &_IID_IAudioClock_c, (void**)&clk->clock);
    if (FAILED(hr)) goto fail;

    hr = clk->client->lpVtbl->GetService(clk->client, &_IID_IAudioRenderClient_c, (void**)&clk->renderer);
    if (FAILED(hr)) goto fail;

    hwclock_reset(clk, 0.0);
    clk->client->lpVtbl->Start(clk->client);
    clk->running = 1;
    return clk;

fail:
    hwclock_destroy(clk);
    return NULL;
}

double hwclock_get_time(HWClock* clk) {
    if (!clk || !clk->running || !clk->clock) return -1.0;
    EnterCriticalSection(&clk->lock);

    UINT64 device_pos = 0;
    clk->clock->lpVtbl->GetPosition(clk->clock, &device_pos, NULL);
    double device_time = (double)device_pos / (double)clk->device_freq;

    UINT32 padding = 0;
    clk->client->lpVtbl->GetCurrentPadding(clk->client, &padding);
    double hw_delay = (double)padding / (double)clk->device_freq;

    double result = clk->pts_base + (device_time - clk->device_base) - hw_delay;
    LeaveCriticalSection(&clk->lock);
    return result;
}

int hwclock_write(HWClock* clk, const uint8_t* pcm, int size, double pts) {
    if (!clk || !clk->running) return -1;
    int total_frames = size / clk->frame_bytes;
    int offset = 0;

    while (total_frames > 0 && clk->running) {
        UINT32 padding = 0;
        clk->client->lpVtbl->GetCurrentPadding(clk->client, &padding);
        UINT32 available = clk->buffer_frames - padding;
        if (available == 0) { Sleep(1); continue; }
        if ((int)available > total_frames) available = total_frames;

        BYTE* buf = NULL;
        clk->renderer->lpVtbl->GetBuffer(clk->renderer, available, &buf);
        int copy_bytes = available * clk->frame_bytes;
        memcpy(buf, pcm + offset, copy_bytes);
        clk->renderer->lpVtbl->ReleaseBuffer(clk->renderer, available, 0);

        EnterCriticalSection(&clk->lock);
        UINT64 dev_pos;
        clk->clock->lpVtbl->GetPosition(clk->clock, &dev_pos, NULL);
        UINT32 pad2 = 0;
        clk->client->lpVtbl->GetCurrentPadding(clk->client, &pad2);
        clk->device_base = (double)dev_pos / (double)clk->device_freq;
        clk->pts_base = pts + (double)pad2 / (double)clk->device_freq;
        LeaveCriticalSection(&clk->lock);

        offset += copy_bytes;
        total_frames -= available;
        pts += (double)available / (double)clk->device_freq;
    }
    return 0;
}

void hwclock_reset(HWClock* clk, double new_pts) {
    if (!clk || !clk->clock) return;
    EnterCriticalSection(&clk->lock);
    UINT64 dev_pos;
    clk->clock->lpVtbl->GetPosition(clk->clock, &dev_pos, NULL);
    clk->device_base = (double)dev_pos / (double)clk->device_freq;
    clk->pts_base = new_pts;
    LeaveCriticalSection(&clk->lock);
}

void hwclock_flush(HWClock* clk) {
    if (!clk || !clk->client) return;
    clk->client->lpVtbl->Stop(clk->client);
    clk->client->lpVtbl->Reset(clk->client);
    hwclock_reset(clk, 0.0);
    clk->client->lpVtbl->Start(clk->client);
}

double hwclock_get_latency(HWClock* clk) {
    return clk ? clk->latency : 0.0;
}

void hwclock_destroy(HWClock* clk) {
    if (!clk) return;
    clk->running = 0;
    if (clk->client) clk->client->lpVtbl->Stop(clk->client);
    if (clk->renderer) clk->renderer->lpVtbl->Release(clk->renderer);
    if (clk->clock) clk->clock->lpVtbl->Release(clk->clock);
    if (clk->client) clk->client->lpVtbl->Release(clk->client);
    DeleteCriticalSection(&clk->lock);
    free(clk);
}

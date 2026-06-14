#include "audio_out.h"
#include "../util/log.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const GUID _CLSID_MMDeviceEnumerator_local =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID _IID_IMMDeviceEnumerator_local =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID _IID_IAudioClient_local =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID _IID_IAudioRenderClient_local =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const IID _IID_IAudioClock_local =
    {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};

/* PKEY_AudioEngine_DeviceFormat = {f19f064d-082c-4e27-bc73-6882a1981a90}, PID 0 */
static const PROPERTYKEY _PKEY_AudioEngine_DeviceFormat_local =
    {{0xf19f064d, 0x082c, 0x4e27, {0xbc,0x73,0x68,0x82,0xa1,0x98,0x1a,0x90}}, 0};

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#define RING_SIZE   (1024 * 1024)
#define MAX_SPEED   1.01
#define MIN_SPEED   0.99

struct AudioOut {
    IAudioClient*       client;
    IAudioRenderClient* render;
    IAudioClock*        clock;
    HANDLE              event;
    HANDLE              thread;
    volatile int        playing;
    int                 exclusive;

    int in_rate;
    int in_channels;
    int in_bits;
    int in_frame_bytes;

    int out_rate;
    int out_channels;
    int out_bits;
    int out_frame_bytes;
    int bytes_per_sec;

    int buffer_frames;

    uint8_t* ring;
    int      ring_size;
    volatile int ring_head;
    volatile int ring_tail;

    double speed;
    double resample_frac;

    uint8_t* tmp_buf;
    int      tmp_buf_size;

    volatile LONG64 total_bytes_written;
    double last_write_pts;
    int    last_write_size;
};

static int ring_avail(AudioOut* ao) {
    int h = ao->ring_head, t = ao->ring_tail;
    return (h - t + ao->ring_size) % ao->ring_size;
}

static int ring_free(AudioOut* ao) {
    return ao->ring_size - 1 - ring_avail(ao);
}

static void ring_write(AudioOut* ao, const uint8_t* data, int size) {
    int h = ao->ring_head;
    while (size > 0) {
        int chunk = ao->ring_size - h;
        if (chunk > size) chunk = size;
        memcpy(ao->ring + h, data, chunk);
        h = (h + chunk) % ao->ring_size;
        data += chunk;
        size -= chunk;
    }
    ao->ring_head = h;
}

static float read_sample(const uint8_t* p, int bits) {
    switch (bits) {
    case 16: {
        int16_t v = (int16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
        return v / 32768.0f;
    }
    case 24: {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v |= (int32_t)0xFF000000;
        return v / 8388608.0f;
    }
    case 32:
        return *(const float*)p;
    default:
        return 0;
    }
}

static void fill_buffer(AudioOut* ao, BYTE* out, int out_frames) {
    float* out_f = (float*)out;
    int out_ch = ao->out_channels;
    int in_ch  = ao->in_channels;
    int in_fb  = ao->in_frame_bytes;

    double ratio = (double)ao->in_rate / ao->out_rate * ao->speed;

    int avail = ring_avail(ao);
    int in_needed = (int)(out_frames * ratio) + 1;
    int in_bytes = in_needed * in_fb;
    if (in_bytes > avail) {
        in_bytes = avail - (avail % in_fb);
        in_needed = in_bytes / in_fb;
    }
    if (in_bytes > ao->tmp_buf_size) {
        in_bytes = ao->tmp_buf_size - (ao->tmp_buf_size % in_fb);
        in_needed = in_bytes / in_fb;
    }
    if (in_needed < 2) {
        memset(out_f, 0, out_frames * out_ch * sizeof(float));
        return;
    }

    uint8_t* tmp = ao->tmp_buf;
    int t = ao->ring_tail;
    int left = in_bytes;
    uint8_t* dst = tmp;
    while (left > 0) {
        int chunk = ao->ring_size - t;
        if (chunk > left) chunk = left;
        memcpy(dst, ao->ring + t, chunk);
        t = (t + chunk) % ao->ring_size;
        dst += chunk;
        left -= chunk;
    }

    int written = 0;
    double pos = ao->resample_frac;

    while (written < out_frames) {
        int idx = (int)pos;
        if (idx >= in_needed - 1) break;
        double frac = pos - idx;

        for (int ch = 0; ch < out_ch; ch++) {
            int ic = ch < in_ch ? ch : 0;
            float s0 = read_sample(tmp + idx * in_fb + ic * (ao->in_bits / 8), ao->in_bits);
            float s1 = read_sample(tmp + (idx + 1) * in_fb + ic * (ao->in_bits / 8), ao->in_bits);
            out_f[written * out_ch + ch] = s0 + (s1 - s0) * (float)frac;
        }

        pos += ratio;
        written++;
    }

    int consumed = (int)pos;
    if (consumed > 0 && consumed < in_needed)
        ao->ring_tail = (ao->ring_tail + consumed * in_fb) % ao->ring_size;
    ao->resample_frac = pos - consumed;

    if (written < out_frames)
        memset(out_f + written * out_ch, 0, (out_frames - written) * out_ch * sizeof(float));
}

static DWORD WINAPI playback_thread(LPVOID arg) {
    AudioOut* ao = (AudioOut*)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    int prebuf = ao->buffer_frames * ao->out_frame_bytes;

    while (ao->playing) {
        DWORD wr = WaitForSingleObject(ao->event, 1000);
        if (!ao->playing) break;
        if (wr != WAIT_OBJECT_0) continue;

        if (ring_avail(ao) < prebuf) {
            UINT32 padding = 0;
            ao->client->lpVtbl->GetCurrentPadding(ao->client, &padding);
            UINT32 frames = ao->buffer_frames - padding;
            if (frames > 0) {
                BYTE* buf = NULL;
                if (SUCCEEDED(ao->render->lpVtbl->GetBuffer(ao->render, frames, &buf)) && buf) {
                    memset(buf, 0, frames * ao->out_frame_bytes);
                    ao->render->lpVtbl->ReleaseBuffer(ao->render, frames, 0);
                }
            }
            continue;
        }

        UINT32 padding = 0;
        ao->client->lpVtbl->GetCurrentPadding(ao->client, &padding);
        UINT32 frames = ao->buffer_frames - padding;
        if (frames == 0) continue;

        BYTE* buf = NULL;
        if (SUCCEEDED(ao->render->lpVtbl->GetBuffer(ao->render, frames, &buf)) && buf) {
            fill_buffer(ao, buf, frames);
            ao->render->lpVtbl->ReleaseBuffer(ao->render, frames, 0);
        }
    }

    CoUninitialize();
    return 0;
}

AudioOut* ao_create(int sample_rate, int channels, int bits) {
    LOG_INFO("WASAPI ao_create: %d Hz, %d ch, %d bit", sample_rate, channels, bits);
    AudioOut* ao = (AudioOut*)calloc(1, sizeof(AudioOut));
    if (!ao) return NULL;

    ao->speed = 1.0;
    ao->in_rate     = sample_rate;
    ao->in_channels = channels;
    ao->in_bits     = bits;
    ao->in_frame_bytes = channels * bits / 8;

    ao->ring_size = RING_SIZE;
    ao->ring = (uint8_t*)malloc(RING_SIZE);
    if (!ao->ring) { free(ao); return NULL; }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* dev_enum = NULL;
    HRESULT hr = CoCreateInstance(&_CLSID_MMDeviceEnumerator_local, NULL, CLSCTX_ALL,
                               &_IID_IMMDeviceEnumerator_local, (void**)&dev_enum);
    if (FAILED(hr)) { LOG_ERROR("CoCreateInstance(MMDeviceEnumerator): 0x%08lX", hr); free(ao->ring); free(ao); return NULL; }

    IMMDevice* device = NULL;
    hr = dev_enum->lpVtbl->GetDefaultAudioEndpoint(dev_enum, eRender, eConsole, &device);
    dev_enum->lpVtbl->Release(dev_enum);
    if (FAILED(hr)) { LOG_ERROR("GetDefaultAudioEndpoint: 0x%08lX", hr); free(ao->ring); free(ao); return NULL; }

    hr = device->lpVtbl->Activate(device, &_IID_IAudioClient_local, CLSCTX_ALL, NULL, (void**)&ao->client);
    if (FAILED(hr)) { LOG_ERROR("IMMDevice_Activate: 0x%08lX", hr); device->lpVtbl->Release(device); free(ao->ring); free(ao); return NULL; }

    /* Try exclusive mode first with mix format */
    WAVEFORMATEX* mix = NULL;
    hr = ao->client->lpVtbl->GetMixFormat(ao->client, &mix);
    if (FAILED(hr)) { LOG_ERROR("GetMixFormat: 0x%08lX", hr); device->lpVtbl->Release(device); ao->client->lpVtbl->Release(ao->client); free(ao->ring); free(ao); return NULL; }

    ao->exclusive = 0;
    hr = ao->client->lpVtbl->Initialize(ao->client,
                                          AUDCLNT_SHAREMODE_EXCLUSIVE,
                                          AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                          (REFERENCE_TIME)200000,
                                          (REFERENCE_TIME)0,
                                          mix,
                                          NULL);
    if (FAILED(hr)) {
        LOG_WARN("Exclusive with mix format failed: 0x%08lX, trying PCM16", hr);
        /* Try 16-bit PCM at mix sample rate */
        WAVEFORMATEX pcm16 = {0};
        pcm16.wFormatTag = WAVE_FORMAT_PCM;
        pcm16.nChannels = mix->nChannels;
        pcm16.nSamplesPerSec = mix->nSamplesPerSec;
        pcm16.wBitsPerSample = 16;
        pcm16.nBlockAlign = pcm16.nChannels * pcm16.wBitsPerSample / 8;
        pcm16.nAvgBytesPerSec = pcm16.nSamplesPerSec * pcm16.nBlockAlign;
        pcm16.cbSize = 0;
        hr = ao->client->lpVtbl->Initialize(ao->client,
                                              AUDCLNT_SHAREMODE_EXCLUSIVE,
                                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                              (REFERENCE_TIME)200000,
                                              (REFERENCE_TIME)0,
                                              &pcm16,
                                              NULL);
    }

    if (SUCCEEDED(hr)) {
        ao->exclusive = 1;
        /* Re-query the actual format used */
        WAVEFORMATEX* actual = NULL;
        ao->client->lpVtbl->GetMixFormat(ao->client, &actual);
        if (actual) {
            ao->out_rate      = actual->nSamplesPerSec;
            ao->out_channels  = actual->nChannels;
            ao->out_bits      = actual->wBitsPerSample;
            ao->out_frame_bytes = actual->nBlockAlign;
            ao->bytes_per_sec = actual->nAvgBytesPerSec;
            CoTaskMemFree(actual);
        } else {
            ao->out_rate      = mix->nSamplesPerSec;
            ao->out_channels  = mix->nChannels;
            ao->out_bits      = 16;
            ao->out_frame_bytes = mix->nChannels * 2;
            ao->bytes_per_sec = mix->nSamplesPerSec * mix->nChannels * 2;
        }
        CoTaskMemFree(mix);
        LOG_INFO("WASAPI: EXCLUSIVE mode %d Hz, %d ch, %d bit", ao->out_rate, ao->out_channels, ao->out_bits);
    } else {
        LOG_WARN("WASAPI exclusive failed: 0x%08lX, falling back to shared", hr);
        /* Re-init client for shared mode */
        ao->client->lpVtbl->Release(ao->client);
        ao->client = NULL;
        hr = device->lpVtbl->Activate(device, &_IID_IAudioClient_local, CLSCTX_ALL, NULL, (void**)&ao->client);
        if (FAILED(hr)) { LOG_ERROR("Re-Activate: 0x%08lX", hr); CoTaskMemFree(mix); device->lpVtbl->Release(device); free(ao->ring); free(ao); return NULL; }

        ao->out_rate      = mix->nSamplesPerSec;
        ao->out_channels  = mix->nChannels;
        ao->out_bits      = mix->wBitsPerSample;
        ao->out_frame_bytes = mix->nBlockAlign;
        ao->bytes_per_sec = mix->nAvgBytesPerSec;

        hr = ao->client->lpVtbl->Initialize(ao->client,
                                              AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                              (REFERENCE_TIME)200000,
                                              (REFERENCE_TIME)0,
                                              mix,
                                              NULL);
        CoTaskMemFree(mix);
        if (FAILED(hr)) { LOG_ERROR("IAudioClient_Initialize shared: 0x%08lX", hr); device->lpVtbl->Release(device); ao->client->lpVtbl->Release(ao->client); free(ao->ring); free(ao); return NULL; }
        LOG_INFO("WASAPI: SHARED mode %d Hz, %d ch, %d bit", ao->out_rate, ao->out_channels, ao->out_bits);
    }

    device->lpVtbl->Release(device);

    ao->event = CreateEvent(NULL, FALSE, FALSE, NULL);
    hr = ao->client->lpVtbl->SetEventHandle(ao->client, ao->event);
    if (FAILED(hr)) {
        LOG_ERROR("SetEventHandle: 0x%08lX", hr);
        CloseHandle(ao->event); ao->client->lpVtbl->Release(ao->client);
        free(ao->ring); free(ao); return NULL;
    }

    hr = ao->client->lpVtbl->GetService(ao->client, &_IID_IAudioRenderClient_local, (void**)&ao->render);
    if (FAILED(hr)) {
        LOG_ERROR("GetService(IAudioRenderClient): 0x%08lX", hr);
        CloseHandle(ao->event); ao->client->lpVtbl->Release(ao->client);
        free(ao->ring); free(ao); return NULL;
    }

    hr = ao->client->lpVtbl->GetService(ao->client, &_IID_IAudioClock_local, (void**)&ao->clock);
    if (FAILED(hr)) {
        LOG_ERROR("GetService(IAudioClock): 0x%08lX", hr);
        ao->render->lpVtbl->Release(ao->render);
        CloseHandle(ao->event); ao->client->lpVtbl->Release(ao->client);
        free(ao->ring); free(ao); return NULL;
    }

    ao->client->lpVtbl->GetBufferSize(ao->client, (UINT32*)&ao->buffer_frames);
    LOG_INFO("WASAPI buffer: %d frames", ao->buffer_frames);

    hr = ao->client->lpVtbl->Start(ao->client);
    if (FAILED(hr)) {
        LOG_ERROR("IAudioClient_Start: 0x%08lX", hr);
        ao->clock->lpVtbl->Release(ao->clock);
        ao->render->lpVtbl->Release(ao->render);
        CloseHandle(ao->event); ao->client->lpVtbl->Release(ao->client);
        free(ao->ring); free(ao); return NULL;
    }

    ao->tmp_buf_size = ao->buffer_frames * 2 * ao->in_frame_bytes;
    ao->tmp_buf = (uint8_t*)malloc(ao->tmp_buf_size);

    ao->playing = 1;
    ao->thread = CreateThread(NULL, 0, playback_thread, ao, 0, NULL);

    LOG_INFO("WASAPI: %d Hz -> %d Hz, %d ch -> %d ch, in %d bit, out %d bit",
             sample_rate, ao->out_rate, channels, ao->out_channels, bits, ao->out_bits);
    return ao;
}

void ao_write(AudioOut* ao, const uint8_t* data, int size) {
    if (!ao || !ao->ring) return;
    int avail = ring_free(ao);
    int to_write = size < avail ? size : avail;
    to_write -= to_write % ao->in_frame_bytes;
    if (to_write > 0) {
        ring_write(ao, data, to_write);
        ao->total_bytes_written += to_write;
        ao->last_write_size = to_write;
    }
}

void ao_set_pts(AudioOut* ao, double pts) {
    if (!ao) return;
    ao->last_write_pts = pts;
}

double ao_get_clock(AudioOut* ao) {
    if (!ao) return 0;
    /* PTS-based clock: last written PTS minus buffered time */
    if (ao->last_write_pts > 0 && ao->bytes_per_sec > 0) {
        double buffered_sec = (double)ring_avail(ao) / ao->bytes_per_sec;
        double clk = ao->last_write_pts - buffered_sec;
        return clk > 0 ? clk : 0;
    }
    /* Fallback: WASAPI hardware clock */
    if (!ao->clock) return 0;
    UINT64 pos = 0;
    if (FAILED(ao->clock->lpVtbl->GetPosition(ao->clock, &pos, NULL)))
        return 0;
    return (double)pos / ao->bytes_per_sec;
}

int ao_is_exclusive(AudioOut* ao) {
    return ao ? ao->exclusive : 0;
}

int ao_get_buffered(AudioOut* ao) {
    return ao ? ring_avail(ao) : 0;
}

int ao_get_free(AudioOut* ao) {
    return ao ? ring_free(ao) : 0;
}

int ao_is_playing(AudioOut* ao) {
    return ao && ao->playing;
}

double ao_get_position_sec(AudioOut* ao) {
    return ao_get_clock(ao);
}

void ao_set_speed(AudioOut* ao, double speed) {
    if (!ao) return;
    if (speed < MIN_SPEED) speed = MIN_SPEED;
    if (speed > MAX_SPEED) speed = MAX_SPEED;
    ao->speed = speed;
}

void ao_pause(AudioOut* ao) {
    if (ao && ao->client) ao->client->lpVtbl->Stop(ao->client);
}

void ao_resume(AudioOut* ao) {
    if (ao && ao->client) ao->client->lpVtbl->Start(ao->client);
}

void ao_reset(AudioOut* ao) {
    if (!ao) return;
    if (ao->client) ao->client->lpVtbl->Stop(ao->client);
    ao->ring_head = 0;
    ao->ring_tail = 0;
    ao->resample_frac = 0;
    ao->total_bytes_written = 0;
    ao->last_write_pts = 0;
    ao->last_write_size = 0;
    ao->speed = 1.0;
    if (ao->client) ao->client->lpVtbl->Start(ao->client);
}

void ao_destroy(AudioOut* ao) {
    if (!ao) return;
    ao->playing = 0;
    if (ao->event) SetEvent(ao->event);
    if (ao->thread) { WaitForSingleObject(ao->thread, 3000); CloseHandle(ao->thread); }
    if (ao->clock)  ao->clock->lpVtbl->Release(ao->clock);
    if (ao->render) ao->render->lpVtbl->Release(ao->render);
    if (ao->client) ao->client->lpVtbl->Release(ao->client);
    if (ao->event)  CloseHandle(ao->event);
    if (ao->ring)     free(ao->ring);
    if (ao->tmp_buf)  free(ao->tmp_buf);
    free(ao);
}

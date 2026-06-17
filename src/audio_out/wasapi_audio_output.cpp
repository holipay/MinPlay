#include "wasapi_audio_output.h"
#include "../util/log.h"
#include <cstdlib>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static const CLSID CLSID_MMDeviceEnumerator_LOCAL =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID IID_IMMDeviceEnumerator_LOCAL =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID IID_IAudioClient_LOCAL =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const IID IID_IAudioRenderClient_LOCAL =
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const IID IID_IAudioClock_LOCAL =
    {0xCD63314F, 0x3FBA, 0x4A1B, {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};

int WasapiAudioOutput::RingAvail() const {
    int h = ring_head_.load(std::memory_order_acquire);
    int t = ring_tail_.load(std::memory_order_acquire);
    return (h - t + ring_size_) % ring_size_;
}

int WasapiAudioOutput::RingFree() const {
    return ring_size_ - 1 - RingAvail();
}

void WasapiAudioOutput::RingWrite(const uint8_t* data, int size) {
    int h = ring_head_.load(std::memory_order_relaxed);
    while (size > 0) {
        int chunk = ring_size_ - h;
        if (chunk > size) chunk = size;
        memcpy(ring_ + h, data, chunk);
        h = (h + chunk) % ring_size_;
        data += chunk;
        size -= chunk;
    }
    ring_head_.store(h, std::memory_order_release);
}

float WasapiAudioOutput::ReadSample(const uint8_t* p, int bits) const {
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
    case 32: {
        float v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    default:
        return 0;
    }
}

void WasapiAudioOutput::FillBuffer(BYTE* out, int out_frames) {
    float* out_f = (float*)out;
    int out_ch = out_channels_;
    int in_ch  = in_channels_;
    int in_fb  = in_frame_bytes_;

    double ratio = (double)in_rate_ / out_rate_ * speed_;

    int avail = RingAvail();
    int in_needed = (int)(out_frames * ratio) + 1;
    int in_bytes = in_needed * in_fb;

    if (in_bytes > avail) {
        in_bytes = avail - (avail % in_fb);
        in_needed = in_bytes / in_fb;
    }
    if (in_bytes > tmp_buf_size_) {
        in_bytes = tmp_buf_size_ - (tmp_buf_size_ % in_fb);
        in_needed = in_bytes / in_fb;
    }
    if (in_needed < 2) {
        if (avail >= in_fb) {
            // Drain the last frame(s) without interpolation
            int consume = avail - (avail % in_fb);
            ring_tail_.store((ring_tail_.load(std::memory_order_relaxed) + consume) % ring_size_, std::memory_order_release);
        }
        resample_frac_ = 0;
        if (out_frames > 0)
            memset(out_f, 0, out_frames * out_ch * sizeof(float));
        return;
    }

    uint8_t* tmp = tmp_buf_;
    int t = ring_tail_.load(std::memory_order_relaxed);
    int left = in_bytes;
    uint8_t* dst = tmp;
    while (left > 0) {
        int chunk = ring_size_ - t;
        if (chunk > left) chunk = left;
        memcpy(dst, ring_ + t, chunk);
        t = (t + chunk) % ring_size_;
        dst += chunk;
        left -= chunk;
    }

    int written = 0;
    double pos = resample_frac_;

    while (written < out_frames) {
        int idx = (int)pos;
        if (idx >= in_needed - 1) break;
        double frac = pos - idx;

        for (int ch = 0; ch < out_ch; ch++) {
            int ic = ch < in_ch ? ch : 0;
            const uint8_t* base = tmp + (idx * in_fb) + ic * (in_bits_ / 8);
            float s0 = ReadSample(base, in_bits_);
            float s1 = ReadSample(base + in_fb, in_bits_);
            float sm1 = (idx > 0) ? ReadSample(base - in_fb, in_bits_) : s0;
            float s2  = (idx < in_needed - 2) ? ReadSample(base + in_fb * 2, in_bits_) : s1;
            float ft = (float)frac;
            float t2 = ft * ft;
            float t3 = t2 * ft;
            out_f[written * out_ch + ch] =
                (-0.5f * sm1 + 1.5f * s0 - 1.5f * s1 + 0.5f * s2) * t3
              + (sm1 - 2.5f * s0 + 2.0f * s1 - 0.5f * s2) * t2
              + (-0.5f * sm1 + 0.5f * s1) * ft
              + s0;
        }
        pos += ratio;
        written++;
    }

    int consumed = (int)pos;
    int to_advance = (consumed > 0) ? (std::min)(consumed, in_needed) : 0;
    if (to_advance > 0)
        ring_tail_.store((ring_tail_.load(std::memory_order_relaxed) + to_advance * in_fb) % ring_size_, std::memory_order_release);
    resample_frac_ = pos - consumed;

    if (written < out_frames)
        memset(out_f + written * out_ch, 0, (out_frames - written) * out_ch * sizeof(float));
}

DWORD WasapiAudioOutput::PlaybackThreadProc() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int drop_warn_interval = 0;

    while (playing_) {
        DWORD wr = WaitForSingleObject(event_, 200);
        if (!playing_) break;
        if (wr != WAIT_OBJECT_0) continue;

        // Periodic warning for ring buffer overflow
        int64_t df = dropped_frames_.load(std::memory_order_relaxed);
        if (df > 0 && (++drop_warn_interval % 50) == 0) {
            LOG_WARN("Audio ring overflow: %lld bytes dropped (buffer full)", df);
        }

        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            // Client may be momentarily stopped during seek/reset — retry on next wake
            continue;
        }
        UINT32 frames = buffer_frames_ - padding;
        if (frames == 0) continue;

        BYTE* buf = nullptr;
        hr = render_->GetBuffer(frames, &buf);
        if (FAILED(hr) || !buf) continue;

        FillBuffer(buf, frames);
        render_->ReleaseBuffer(frames, 0);
    }

    CoUninitialize();
    return 0;
}

DWORD WINAPI WasapiAudioOutput::PlaybackThread(LPVOID arg) {
    return ((WasapiAudioOutput*)arg)->PlaybackThreadProc();
}

bool WasapiAudioOutput::Initialize(int sample_rate, int channels, int bits) {
    LOG_INFO("WASAPI Initialize: %d Hz, %d ch, %d bit", sample_rate, channels, bits);

    if (channels <= 0 || bits <= 0) {
        LOG_ERROR("Invalid audio params: channels=%d bits=%d", channels, bits);
        return false;
    }
    in_rate_ = sample_rate;
    in_channels_ = channels;
    in_bits_ = bits;
    in_frame_bytes_ = channels * bits / 8;

    ring_ = (uint8_t*)malloc(RING_SIZE);
    if (!ring_) return false;

    IMMDeviceEnumerator* dev_enum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_LOCAL, nullptr,
        CLSCTX_ALL, IID_IMMDeviceEnumerator_LOCAL, (void**)&dev_enum);
    if (FAILED(hr)) { LOG_ERROR("CoCreateInstance(MMDeviceEnumerator): 0x%08lX", hr); free(ring_); ring_ = nullptr; return false; }

    IMMDevice* device = nullptr;
    hr = dev_enum->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    dev_enum->Release();
    if (FAILED(hr)) { LOG_ERROR("GetDefaultAudioEndpoint: 0x%08lX", hr); free(ring_); ring_ = nullptr; return false; }

    hr = device->Activate(IID_IAudioClient_LOCAL, CLSCTX_ALL, nullptr, (void**)&client_);
    if (FAILED(hr)) { LOG_ERROR("IMMDevice_Activate: 0x%08lX", hr); device->Release(); free(ring_); ring_ = nullptr; return false; }

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr)) { LOG_ERROR("GetMixFormat: 0x%08lX", hr); device->Release(); client_->Release(); client_ = nullptr; free(ring_); ring_ = nullptr; return false; }

    exclusive_ = false;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        (REFERENCE_TIME)200000, (REFERENCE_TIME)0, mix, nullptr);
    if (SUCCEEDED(hr)) {
        exclusive_ = true;
        WAVEFORMATEX* actual = nullptr;
        client_->GetMixFormat(&actual);
        if (actual) {
            out_rate_ = actual->nSamplesPerSec;
            out_channels_ = actual->nChannels;
            out_bits_ = actual->wBitsPerSample;
            out_frame_bytes_ = actual->nBlockAlign;
            bytes_per_sec_ = actual->nAvgBytesPerSec;
            CoTaskMemFree(actual);
        } else {
            out_rate_ = mix->nSamplesPerSec;
            out_channels_ = mix->nChannels;
            out_bits_ = 16;
            out_frame_bytes_ = mix->nChannels * 2;
            bytes_per_sec_ = mix->nSamplesPerSec * mix->nChannels * 2;
        }
        CoTaskMemFree(mix);
        LOG_INFO("WASAPI: EXCLUSIVE mode %d Hz, %d ch, %d bit", out_rate_, out_channels_, out_bits_);
    } else {
        LOG_WARN("WASAPI exclusive failed: 0x%08lX, falling back to shared", hr);
        client_->Release();
        client_ = nullptr;
        hr = device->Activate(IID_IAudioClient_LOCAL, CLSCTX_ALL, nullptr, (void**)&client_);
        if (FAILED(hr)) { LOG_ERROR("Re-Activate: 0x%08lX", hr); CoTaskMemFree(mix); device->Release(); free(ring_); ring_ = nullptr; return false; }

        out_rate_ = mix->nSamplesPerSec;
        out_channels_ = mix->nChannels;
        out_bits_ = mix->wBitsPerSample;
        out_frame_bytes_ = mix->nBlockAlign;
        bytes_per_sec_ = mix->nAvgBytesPerSec;

        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            (REFERENCE_TIME)200000, (REFERENCE_TIME)0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) { LOG_ERROR("IAudioClient_Initialize shared: 0x%08lX", hr); device->Release(); client_->Release(); client_ = nullptr; free(ring_); ring_ = nullptr; return false; }
        LOG_INFO("WASAPI: SHARED mode %d Hz, %d ch, %d bit", out_rate_, out_channels_, out_bits_);
    }

    device->Release();

    event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    hr = client_->SetEventHandle(event_);
    if (FAILED(hr)) {
        LOG_ERROR("SetEventHandle: 0x%08lX", hr);
        CloseHandle(event_); event_ = nullptr;
        client_->Release(); client_ = nullptr;
        free(ring_); ring_ = nullptr;
        return false;
    }

    hr = client_->GetService(IID_IAudioRenderClient_LOCAL, (void**)&render_);
    if (FAILED(hr)) {
        LOG_ERROR("GetService(IAudioRenderClient): 0x%08lX", hr);
        CloseHandle(event_); event_ = nullptr;
        client_->Release(); client_ = nullptr;
        free(ring_); ring_ = nullptr;
        return false;
    }

    hr = client_->GetService(IID_IAudioClock_LOCAL, (void**)&clock_);
    if (FAILED(hr)) {
        LOG_ERROR("GetService(IAudioClock): 0x%08lX", hr);
        render_->Release(); render_ = nullptr;
        CloseHandle(event_); event_ = nullptr;
        client_->Release(); client_ = nullptr;
        free(ring_); ring_ = nullptr;
        return false;
    }

    client_->GetBufferSize((UINT32*)&buffer_frames_);
    LOG_INFO("WASAPI buffer: %d frames", buffer_frames_);

    client_->Start();

    tmp_buf_size_ = buffer_frames_ * 2 * in_frame_bytes_;
    tmp_buf_ = (uint8_t*)malloc(tmp_buf_size_);
    if (!tmp_buf_) {
        LOG_ERROR("malloc tmp_buf_ failed (%d)", tmp_buf_size_);
        client_->Stop();
        playing_ = false;
        return false;
    }

    playing_ = true;
    thread_ = CreateThread(nullptr, 0, PlaybackThread, this, 0, nullptr);
    if (!thread_) {
        LOG_ERROR("CreateThread for WASAPI playback failed");
        playing_ = false;
        client_->Stop();
        return false;
    }

    LOG_INFO("WASAPI: %d Hz -> %d Hz, %d ch -> %d ch, in %d bit, out %d bit",
             sample_rate, out_rate_, channels, out_channels_, bits, out_bits_);
    return true;
}

WasapiAudioOutput::~WasapiAudioOutput() {
    playing_ = false;
    if (event_) SetEvent(event_);
    if (thread_) { WaitForSingleObject(thread_, 3000); CloseHandle(thread_); }
    if (clock_)  clock_->Release();
    if (render_) render_->Release();
    if (client_) client_->Release();
    if (event_)  CloseHandle(event_);
    free(ring_);
    free(tmp_buf_);
}

int WasapiAudioOutput::Write(const uint8_t* data, int size) {
    if (!ring_) return 0;
    int avail = RingFree();
    int to_write = size < avail ? size : avail;
    int align_loss = to_write % in_frame_bytes_;
    to_write -= align_loss;
    int dropped = size - to_write;
    if (dropped > 0)
        dropped_frames_.fetch_add(dropped, std::memory_order_relaxed);
    if (to_write > 0) {
        RingWrite(data, to_write);
        total_bytes_written_.fetch_add(to_write, std::memory_order_relaxed);
    }
    return to_write;
}

double WasapiAudioOutput::GetClock() {
    // Hardware clock: sample position from audio device
    if (clock_) {
        UINT64 pos = 0;
        if (SUCCEEDED(clock_->GetPosition(&pos, nullptr))) {
            double hw_sec = (double)pos / out_rate_;
            // Compensate WASAPI buffer latency: GetPosition() reports what has
            // been written to the endpoint, but sound hasn't reached speakers yet.
            double latency = (double)buffer_frames_ / out_rate_;
            double clk = hw_sec - latency;
            return clk > 0 ? clk : 0;
        }
    }
    // Fallback: PTS estimate minus buffered duration (large buffers skew this)
    double pts = last_write_pts_.load(std::memory_order_acquire);
    if (pts > 0 && in_rate_ > 0 && in_frame_bytes_ > 0) {
        double buffered_sec = (double)RingAvail() / (double)(in_rate_ * in_frame_bytes_);
        double clk = pts - buffered_sec;
        return clk > 0 ? clk : 0;
    }
    return 0;
}

int WasapiAudioOutput::GetBuffered() { return RingAvail(); }
int WasapiAudioOutput::GetFree() { return RingFree(); }

void WasapiAudioOutput::Pause() { if (client_) client_->Stop(); }
void WasapiAudioOutput::Resume() { if (client_) client_->Start(); }

void WasapiAudioOutput::Reset() {
    // Stop the audio engine — this drains any pending audio and makes
    // GetCurrentPadding return an error, causing the playback thread to
    // skip its iteration and retry on the next wake event.
    if (client_) client_->Stop();

    // Reset ring buffer state (thread is idle because client is stopped)
    ring_head_.store(0, std::memory_order_relaxed);
    ring_tail_.store(0, std::memory_order_relaxed);
    resample_frac_ = 0;
    total_bytes_written_.store(0, std::memory_order_relaxed);
    dropped_frames_.store(0, std::memory_order_relaxed);
    last_write_pts_.store(0.0, std::memory_order_relaxed);
    speed_ = 1.0;

    // Restart audio engine — playback thread resumes naturally
    if (client_) client_->Start();
}

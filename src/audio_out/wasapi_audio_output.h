#pragma once
#include "audio_output.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdint>
#include <atomic>

class WasapiAudioOutput : public AudioOutput {
public:
    WasapiAudioOutput() = default;
    ~WasapiAudioOutput() override;

    WasapiAudioOutput(const WasapiAudioOutput&) = delete;
    WasapiAudioOutput& operator=(const WasapiAudioOutput&) = delete;

    bool Initialize(int sample_rate, int channels, int bits);

    void Write(const uint8_t* data, int size) override;
    void SetPts(double pts) override { last_write_pts_.store(pts, std::memory_order_relaxed); }
    double GetClock() override;
    int GetBuffered() override;
    int GetFree() override;
    bool IsExclusive() override { return exclusive_; }
    void Pause() override;
    void Resume() override;
    void Reset() override;

private:
    static constexpr int RING_SIZE = 4 * 1024 * 1024;

    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    IAudioClock* clock_ = nullptr;
    HANDLE event_ = nullptr;
    HANDLE thread_ = nullptr;
    std::atomic<bool> playing_{false};
    bool exclusive_ = false;

    int in_rate_ = 0;
    int in_channels_ = 0;
    int in_bits_ = 0;
    int in_frame_bytes_ = 0;

    int out_rate_ = 0;
    int out_channels_ = 0;
    int out_bits_ = 0;
    int out_frame_bytes_ = 0;
    int bytes_per_sec_ = 0;
    int buffer_frames_ = 0;

    /*
     * SPSC ring buffer — single producer (MF callback OnReadSample → ao_->Write)
     *                   single consumer (WASAPI playback thread → FillBuffer)
     *
     * head_ = producer write position (release store so consumer sees written data)
     * tail_ = consumer read position (release store so producer sees freed space)
     *
     * No locks: x86 guarantees single-copy atomicity for aligned ints,
     * memory_order_release/acquire provides visibility ordering.
     * RingAvail() loads both with acquire — may see stale head w.r.t. tail
     * but this is benign (conservative lower bound on available data).
     */
    uint8_t* ring_ = nullptr;
    int ring_size_ = RING_SIZE;
    std::atomic<int> ring_head_{0};
    std::atomic<int> ring_tail_{0};

    std::atomic<double> speed_{1.0};
    double resample_frac_ = 0.0;

    uint8_t* tmp_buf_ = nullptr;
    int tmp_buf_size_ = 0;

    std::atomic<int64_t> total_bytes_written_{0};
    std::atomic<double> last_write_pts_{0.0};

    friend struct AudioTest;
    int RingAvail() const;
    int RingFree() const;
    void RingWrite(const uint8_t* data, int size);
    float ReadSample(const uint8_t* p, int bits) const;
    void FillBuffer(BYTE* out, int out_frames);

    static DWORD WINAPI PlaybackThread(LPVOID arg);
    DWORD PlaybackThreadProc();
};

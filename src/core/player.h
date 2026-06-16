#pragma once
#include "../util/com_ptr.h"
#include "../media/media_source.h"
#include "../video_out/video_output.h"
#include "../audio_out/audio_output.h"
#include "../sync/sync_context.h"
#include "../util/osd.h"
#include "source_reader_callback.h"
#include <windows.h>
#include <mfapi.h>
#include <cstdint>
#include <mutex>
#include <atomic>

#define TIMER_AUDIO_CHECK   1
#define TIMER_VIDEO_DISPLAY 2
#define TIMER_EOF_CHECK     3

// Audio buffering thresholds
constexpr int AUDIO_BUFFER_NETWORK_MULT = 5;   // 5 × bitrate for network (5 s buffer)
constexpr int AUDIO_BUFFER_LOCAL_DIV   = 5;   // bitrate / 5 for local (200 ms buffer)

// Video queue fill targets (out of VQ_SIZE = 32)
constexpr int VIDEO_FILL_NETWORK = 15;  // keep 15+ frames for network
constexpr int VIDEO_FILL_LOCAL   = 1;   // 1+ frame for local files

// A/V sync defaults
constexpr double SYNC_WINDOW_DEFAULT   = 0.020;  // 20 ms tolerance
constexpr double SYNC_WINDOW_EXCLUSIVE = 0.010;  // 10 ms for exclusive-mode WASAPI

enum class PlayerState : int { Stopped, Playing, Paused };

class Player {
public:
    Player();
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    bool Open(HWND hwnd, const wchar_t* url);
    void Close();
    void Destroy();

    void Play();
    void PauseToggle();
    void Seek(double seconds);

    void Resize(int w, int h);
    void Paint(HDC hdc, int w, int h);
    void VideoTick();
    void CheckAudio();
    void ProcessVideoFrame(IMFSample* sample, LONGLONG timestamp);

    PlayerState GetState() const { return state_.load(std::memory_order_relaxed); }
    HWND GetHwnd() const { return hwnd_; }
    double GetDuration() const;
    double GetPosition() const;
    bool HasVideo() const { return has_video_; }
    bool HasAudio() const { return has_audio_; }
    double GetVideoFps() const { return video_fps_.load(std::memory_order_relaxed); }
    bool IsFinished() const;
    void OnVideoFormatChanged();

private:
    struct VFrame {
        uint8_t* data = nullptr;
        int size = 0;
        double timestamp = 0;
        PixelFormat pix_fmt = PixelFormat::Unknown;
    };

    static constexpr int VQ_SIZE = 32;

    HWND hwnd_ = nullptr;
    std::atomic<PlayerState> state_{PlayerState::Stopped};

    MediaSource* source_ = nullptr;
    SourceReaderCallback* callback_ = nullptr;
    VideoOutput* vo_ = nullptr;
    AudioOutput* ao_ = nullptr;
    SyncContext* sync_ = nullptr;
    OSD* osd_ = nullptr;

    bool has_video_ = false;
    bool has_audio_ = false;
    bool is_network_ = false;
    int audio_bytes_per_sec_ = 0;
    std::atomic<double> video_fps_{30.0};

    LARGE_INTEGER perf_freq_{};
    std::atomic<double> start_time_{0};
    std::atomic<double> pause_offset_{0};
    std::atomic<double> pause_start_{0};

    // Frame queue (producer: MF callback, consumer: main thread)
    // All accesses are under vq_mutex_ — plain int is sufficient
    VFrame vq_[VQ_SIZE];
    int vq_head_ = 0;
    int vq_tail_ = 0;
    mutable std::mutex vq_mutex_;

    // Render frame buffer
    mutable std::mutex frame_mutex_;
    uint8_t* frame_buf_ = nullptr;
    int frame_buf_size_ = 0;
    std::atomic<bool> frame_ready_{false};
    std::atomic<int> frame_w_{0};
    std::atomic<int> frame_h_{0};
    int frame_size_ = 0;
    std::atomic<PixelFormat> pix_fmt_{PixelFormat::Unknown};



    std::atomic<LONG> video_first_frame_post_{0};

    int win_w_ = 0;
    int win_h_ = 0;

    static double GetTimeSec(const LARGE_INTEGER& freq);
    double ElapsedSec() const;

    static uint8_t* ConvertYUY2ToNV12(const uint8_t* yuy2, int w, int h);
    static uint8_t* ConvertI420ToNV12(const uint8_t* i420, int w, int h);
    bool RenderD3D(int w, int h);
};

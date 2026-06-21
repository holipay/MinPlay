#include "player.h"
#include "../media/media_source.h"
#include "../util/yuv_convert.h"
#include "../video_out/d3d11_video_output.h"
#include "../audio_out/wasapi_audio_output.h"
#include "../util/log.h"
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <new>
#include <thread>

Player::Player() {
    QueryPerformanceFrequency(&perf_freq_);
}

Player::~Player() {
    Close();
}

static void OSDOverlayCallback(HDC hdc, int /*w*/, int /*h*/, void* ctx) {
    Player* self = (Player*)ctx;
    self->DrawOSD(hdc);
}

double Player::GetTimeSec(const LARGE_INTEGER& freq) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (double)li.QuadPart / freq.QuadPart;
}

double Player::ElapsedSec() const {
    double now = GetTimeSec(perf_freq_);
    // Snapshot all three atomics once for consistency (all set from main thread)
    double ps = pause_start_.load(std::memory_order_acquire);
    double st = start_time_.load(std::memory_order_acquire);
    double po = pause_offset_.load(std::memory_order_acquire);
    // If paused, freeze at the moment pause was entered
    if (ps > 0)
        return ps - st - po;
    return now - st - po;
}

void Player::StartVideoTimer() {
    if (!has_video_ || !hwnd_) return;
    double fps = video_fps_.load(std::memory_order_acquire);
    if (fps <= 0) fps = 30.0;
    int period = (int)(1000.0 / fps);
    if (period < 1) period = 1;
    SetTimer(hwnd_, TIMER_VIDEO_DISPLAY, period, nullptr);
}

void Player::StopVideoTimer() {
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_VIDEO_DISPLAY);
    }
}

bool Player::Open(HWND hwnd, const wchar_t* url, bool audio_only) {
    Close();
    hwnd_ = hwnd;
    audio_only_ = audio_only;
    saved_url_ = url;

    callback_ = new (std::nothrow) SourceReaderCallback();
    if (!callback_) {
        LOG_CRITICAL("Out of memory: callback_");
        return false;
    }

    osd_ = new (std::nothrow) OSD();
    if (!osd_)
        LOG_WARN("Out of memory: osd_ (OSD overlay disabled)");

    state_.store(PlayerState::Opening, std::memory_order_release);

    open_generation_.fetch_add(1, std::memory_order_relaxed);
    source_generation_.fetch_add(1, std::memory_order_relaxed);
    open_running_ = true;
    int gen = open_generation_.load(std::memory_order_relaxed);
    open_thread_ = std::thread([this, url_str = std::wstring(url), audio_only, gen]() {
        OpenAsync(url_str, audio_only, gen);
    });
    return true;
}

void Player::OpenAsync(std::wstring url, bool audio_only, int generation) {
    source_ = new (std::nothrow) MediaSource();
    if (!source_) {
        LOG_CRITICAL("Out of memory: source_");
        open_ok_ = false;
        PostMessage(hwnd_, WM_OPEN_COMPLETE, 0, generation);
        return;
    }

    if (!source_->Open(url.c_str(), callback_, audio_only)) {
        LOG_ERROR("Failed to open media source");
        open_ok_ = false;
        PostMessage(hwnd_, WM_OPEN_COMPLETE, 0, generation);
        return;
    }

    // Check abort after potentially long network/HLS open
    if (!open_running_.load(std::memory_order_acquire)) {
        LOG_INFO("OpenAsync: aborted after source open");
        open_ok_ = false;
        source_->Close(); delete source_; source_ = nullptr;
        PostMessage(hwnd_, WM_OPEN_COMPLETE, 0, generation);
        return;
    }

    has_video_ = source_->HasVideo();
    has_audio_ = source_->HasAudio();
    is_network_ = (wcsncmp(url.c_str(), L"http://", 7) == 0) ||
                  (wcsncmp(url.c_str(), L"https://", 8) == 0);
    pix_fmt_ = source_->GetPixelFormat();

    if (has_video_) {
        VideoInfo vi = source_->GetVideoInfo();
        frame_w_ = vi.width;
        frame_h_ = vi.height;
        stride_ = vi.stride;
        video_fps_ = vi.fps;
    }

    if (has_audio_) {
        AudioInfo ai = source_->GetAudioInfo();
        WasapiAudioOutput* wasapi = new (std::nothrow) WasapiAudioOutput();
        if (!wasapi) {
            LOG_CRITICAL("Out of memory: ao_");
            has_audio_ = false;
        } else if (!wasapi->Initialize(ai.sample_rate, ai.channels, ai.bits_per_sample)) {
            LOG_ERROR("Failed to initialize audio output");
            delete wasapi;
            has_audio_ = false;
        } else {
            ao_ = wasapi;
            audio_bytes_per_sec_ = ai.sample_rate * ai.channels * ((ai.bits_per_sample + 7) / 8);
            callback_->SetAudioOutput(ao_);
        }
        if (!has_audio_ && !source_->HasVideo()) {
            LOG_WARN("Audio-only stream lost audio output — cannot play");
            open_ok_ = false;
            PostMessage(hwnd_, WM_OPEN_COMPLETE, 0, generation);
            return;
        }
    }

    // Check abort after audio output init
    if (!open_running_.load(std::memory_order_acquire)) {
        LOG_INFO("OpenAsync: aborted after audio init");
        open_ok_ = false;
        source_->Close(); delete source_; source_ = nullptr;
        PostMessage(hwnd_, WM_OPEN_COMPLETE, 0, generation);
        return;
    }

    {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        win_w_ = rc.right;
        win_h_ = rc.bottom;
    }

    {
        double sync_window = SYNC_WINDOW_DEFAULT;
        if (ao_ && ao_->IsExclusive())
            sync_window = SYNC_WINDOW_EXCLUSIVE;
        sync_ = new (std::nothrow) SyncContext(sync_window);
        if (!sync_) {
            LOG_CRITICAL("Out of memory: sync_");
            open_ok_ = false;
            PostMessage(hwnd_, WM_OPEN_COMPLETE, 0, generation);
            return;
        }
    }

    if (!audio_only) {
        D3D11VideoOutput* d3d_vo = new (std::nothrow) D3D11VideoOutput();
        if (d3d_vo) {
            if (d3d_vo->Initialize(hwnd_, win_w_, win_h_)) {
                d3d_vo->SetOverlay(OSDOverlayCallback, this);
                vo_ = d3d_vo;
            } else {
                LOG_ERROR("Failed to initialize D3D11 video output");
                delete d3d_vo;
            }
        } else {
            LOG_CRITICAL("Out of memory: d3d_vo");
        }
    }

    callback_->SetPlayer(this);
    callback_->SetLive(source_->IsLive() || source_->Duration() <= 0);
    callback_->SetReader(source_->GetReader(),
                          source_->GetVideoStream(),
                          source_->GetAudioStream());

    open_ok_ = true;
    PostMessage(hwnd_, WM_OPEN_COMPLETE, 1, generation);
}

void Player::Close() {
    // Signal async open thread to abort
    open_running_ = false;

    // Wait for background thread to finish before tearing down its objects
    if (open_thread_.joinable())
        open_thread_.join();

    // Kill timers first to prevent WM_TIMER callbacks during teardown
    StopVideoTimer();
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_AUDIO_CHECK);
        KillTimer(hwnd_, TIMER_EOF_CHECK);
    }

    if (callback_) callback_->Stop();

    // Stop audio output before releasing objects
    if (ao_) {
        ao_->Pause();
        ao_->Reset();
    }

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        vq_head_ = 0;
        vq_tail_ = 0;
    }

    if (callback_) callback_->ClearPointers();

    delete vo_;       vo_ = nullptr;
    delete ao_;       ao_ = nullptr;
    delete sync_;     sync_ = nullptr;
    delete osd_;      osd_ = nullptr;

    if (source_) { source_->Close(); delete source_; source_ = nullptr; }
    if (callback_) { callback_->Release(); callback_ = nullptr; }

    state_.store(PlayerState::Stopped, std::memory_order_release);
    for (int i = 0; i < VQ_SIZE; i++) {
        free(vq_[i].data);
        vq_[i].data = nullptr;
    }
    free(frame_buf_);
    frame_buf_ = nullptr;
    frame_buf_size_ = 0;
    free(convert_buf_);
    convert_buf_ = nullptr;
    convert_buf_size_ = 0;
    frame_size_ = 0;
    pix_fmt_.store(PixelFormat::Unknown, std::memory_order_relaxed);
}

void Player::Play() {
    if (!source_) return;
    auto s = state_.load(std::memory_order_acquire);
    if (s != PlayerState::Stopped && s != PlayerState::Opening) return;

    state_.store(PlayerState::Playing, std::memory_order_release);
    start_time_.store(GetTimeSec(perf_freq_), std::memory_order_release);
    pause_offset_.store(0, std::memory_order_release);
    pause_start_.store(0, std::memory_order_release);

    video_first_frame_post_.store(1, std::memory_order_release);
    if (ao_) ao_->Resume();
    if (callback_) callback_->StartReading();
    StartVideoTimer();
    if (has_video_)
        PostMessage(hwnd_, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
}

void Player::PlayCurrentTrack() {
    if (!playlist_) return;
    const auto& entry = playlist_->GetCurrent();
    LOG_INFO("Playlist: playing track %d/%d — %S",
             playlist_->GetIndex() + 1, playlist_->GetCount(),
             entry.title.empty() ? entry.url.c_str() : entry.title.c_str());
    Close();
    Open(hwnd_, entry.url.c_str(), audio_only_);
}

void Player::OnTrackFinished() {
    if (!playlist_ || !playlist_->HasNext()) {
        PostMessage(hwnd_, WM_PLAYLIST_DONE, 0, 0);
        return;
    }
    playlist_->Next();
    PlayCurrentTrack();
}

bool Player::PlayNext() {
    if (!playlist_ || !playlist_->HasNext()) return false;
    playlist_->Next();
    PlayCurrentTrack();
    return true;
}

bool Player::PlayPrev() {
    if (!playlist_ || !playlist_->HasPrev()) return false;
    playlist_->Prev();
    PlayCurrentTrack();
    return true;
}

void Player::PauseToggle() {
    if (state_.load(std::memory_order_acquire) == PlayerState::Playing) {
        state_.store(PlayerState::Paused, std::memory_order_release);
        pause_start_.store(GetTimeSec(perf_freq_), std::memory_order_release);
    if (callback_) {
        callback_->Stop();
        // Wait for any in-flight OnReadSample to finish (may hold stale ao_ pointer)
        DWORD wait_start = GetTickCount();
        while (callback_->IsBusy() && (GetTickCount() - wait_start) < 3000) {
            Sleep(10);
        }
        callback_->ClearPointers();
    }
        if (ao_) ao_->Pause();
        StopVideoTimer();
    } else if (state_.load(std::memory_order_acquire) == PlayerState::Paused) {
        // Accumulate pause duration, don't overwrite.
        // pause_offset_ += (now - pause_start_)
        double now = GetTimeSec(perf_freq_);
        double prev = pause_offset_.load(std::memory_order_acquire);
        double ps = pause_start_.load(std::memory_order_acquire);
        if (ps > 0) {
            pause_offset_.store(prev + (now - ps), std::memory_order_release);
            pause_start_.store(0, std::memory_order_release);
        }
        state_.store(PlayerState::Playing, std::memory_order_release);
        if (ao_) ao_->Resume();
        if (callback_) callback_->StartReading();
        StartVideoTimer();
        if (has_video_)
            PostMessage(hwnd_, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
    }
}

void Player::Seek(double seconds) {
    if (!source_) return;
    if (source_->IsLive()) return;
    if (seconds < 0) seconds = 0;
    auto s = state_.load(std::memory_order_acquire);
    bool was_playing = (s == PlayerState::Playing);
    bool was_paused  = (s == PlayerState::Paused);

    if (callback_) callback_->Stop();
    if (ao_) ao_->Reset();
    StopVideoTimer();

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        for (int i = 0; i < VQ_SIZE; i++) {
            free(vq_[i].data);
            vq_[i].data = nullptr;
            vq_[i].size = 0;
        }
        vq_head_ = 0;
        vq_tail_ = 0;
    }

    source_->Seek(seconds);

    frame_ready_ = false;

    // Update time base so ElapsedSec() returns the seeked position
    double now = GetTimeSec(perf_freq_);
    if (was_paused) {
        // During pause, ElapsedSec = pause_start_ - start_time_ - pause_offset_
        // We want: seconds = pause_start_ - new_start_time_ - pause_offset_
        double po = pause_offset_.load(std::memory_order_acquire);
        double ps = pause_start_.load(std::memory_order_acquire);
        start_time_.store(ps - seconds - po, std::memory_order_release);
        // pause_offset_ and pause_start_ stay unchanged — ElapsedSec will still freeze
        // at the correct new position because pause_start_ hasn't moved.
        // Actually we need to adjust: currently ElapsedSec() = ps - start_time - po
        // With new start_time = ps - seconds - po:
        //   ElapsedSec = ps - (ps - seconds - po) - po = seconds ✓
    } else {
        start_time_.store(now - seconds, std::memory_order_release);
        pause_offset_.store(0, std::memory_order_release);
        pause_start_.store(0, std::memory_order_release);
    }

    video_first_frame_post_.store(1, std::memory_order_release);

    if (was_playing) {
        state_.store(PlayerState::Playing, std::memory_order_release);
        if (ao_) ao_->Resume();
        if (callback_) callback_->StartReading();
        StartVideoTimer();
        if (has_video_)
            PostMessage(hwnd_, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
    } else if (was_paused) {
        // Stay paused — just post a Paint so OSD updates
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

double Player::GetDuration() const {
    if (source_ && source_->IsLive()) return -1;
    if (!source_) return -1;
    return source_->Duration();
}

double Player::GetPosition() const {
    double pos = ElapsedSec();
    double dur = GetDuration();
    if (dur > 0 && pos > dur) pos = dur;
    return pos < 0 ? 0 : pos;
}

void Player::SetVolume(float vol) {
    if (ao_) ao_->SetVolume(vol);
}

float Player::GetVolume() const {
    return ao_ ? ao_->GetVolume() : 1.0f;
}

void Player::ToggleMute() {
    if (ao_) ao_->SetMuted(!ao_->IsMuted());
}

bool Player::IsMuted() const {
    return ao_ ? ao_->IsMuted() : false;
}

void Player::ToggleFullscreen() {
    if (!hwnd_) return;

    if (is_fullscreen_) {
        // Exit fullscreen
        SetWindowLong(hwnd_, GWL_STYLE, saved_style_ | WS_VISIBLE);
        SetWindowLong(hwnd_, GWL_EXSTYLE, saved_ex_style_);
        SetWindowPlacement(hwnd_, &saved_wpl_);
        ShowCursor(TRUE);
        is_fullscreen_ = false;
    } else {
        // Enter fullscreen
        GetWindowPlacement(hwnd_, &saved_wpl_);
        saved_style_ = GetWindowLong(hwnd_, GWL_STYLE);
        saved_ex_style_ = GetWindowLong(hwnd_, GWL_EXSTYLE);

        SetWindowLong(hwnd_, GWL_STYLE,
            (saved_style_ & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX))
            | WS_POPUP | WS_VISIBLE);
        SetWindowLong(hwnd_, GWL_EXSTYLE, saved_ex_style_ | WS_EX_TOPMOST);

        MONITORINFO mi = { sizeof(mi) };
        HMONITOR hmon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        if (GetMonitorInfo(hmon, &mi)) {
            RECT r = mi.rcMonitor;
            SetWindowPos(hwnd_, HWND_TOPMOST,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_FRAMECHANGED);
        }
        ShowCursor(FALSE);
        is_fullscreen_ = true;
    }
}

void Player::Resize(int w, int h) {
    win_w_ = w;
    win_h_ = h;
}

void Player::Paint(HDC hdc, int /*w*/, int /*h*/) {
    DrawOSD(hdc);
}

void Player::DrawOSD(HDC hdc) {
    if (osd_) {
        const wchar_t* track_title = nullptr;
        int track_idx = -1;
        int track_count = 0;
        if (playlist_ && playlist_->GetCount() > 0) {
            track_idx = playlist_->GetIndex();
            track_count = playlist_->GetCount();
            const auto& entry = playlist_->GetCurrent();
            if (!entry.title.empty())
                track_title = entry.title.c_str();
        }
        osd_->Draw(hdc, GetPosition(), GetDuration(),
                   (int)video_fps_.load(std::memory_order_relaxed),
                   ao_ ? ao_->GetVolume() : 1.0f,
                   ao_ ? ao_->IsMuted() : false,
                   track_idx, track_count, track_title);
    }
}

bool Player::IsFinished() const {
    auto s = state_.load(std::memory_order_acquire);
    if (s != PlayerState::Playing && s != PlayerState::Paused) return false;
    if (source_ && (source_->IsLive() || source_->Duration() <= 0)) return false;
    bool vq_empty;
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        vq_empty = (vq_head_ == vq_tail_);
    }
    bool f_ready;
    {
        f_ready = frame_ready_;
    }
    bool vdone  = audio_only_ || !has_video_ || (callback_ && callback_->IsVideoEof() && vq_empty && !f_ready);
    bool adone  = !has_audio_ || (callback_ && callback_->IsAudioEof() && ao_ && ao_->GetBuffered() == 0);
    if (vdone && adone) {
        // Debounce: require two consecutive true checks to avoid closing
        // before the last rendered frame has been on screen for one frame
        // interval (timer is 500ms, so this adds at most 500ms).
        if (++finished_debounce_ >= 2) return true;
    } else {
        finished_debounce_ = 0;
    }
    return false;
}

uint8_t* Player::ConvertYUY2ToNV12(const uint8_t* yuy2, int w, int h) {
    return ::ConvertYUY2ToNV12(yuy2, w, h);
}

uint8_t* Player::ConvertI420ToNV12(const uint8_t* i420, int w, int h) {
    return ::ConvertI420ToNV12(i420, w, h);
}

void Player::TryRestartLivePipeline() {
    if (!source_ || !callback_) return;

    bool is_live_or_unknown = source_->IsLive() || source_->Duration() <= 0;
    bool is_network_vod = is_network_ && !is_live_or_unknown;

    if (!is_live_or_unknown && !is_network_vod) return;

    double now = GetTimeSec(perf_freq_);
    double last_frame = last_video_frame_time_.load(std::memory_order_acquire);
    bool stalled = (last_frame > 0 && (now - last_frame) > 5.0);

    if (!stalled) return;

    if (is_live_or_unknown) {
        if (!source_->HlsByteStreamHasData()) return;
        if (!source_->HasNewHlsData()) return;
    }

    LOG_INFO("Live/VOD: pipeline stalled for %.1fs, attempting recovery", now - last_frame);

    double last_restart = last_restart_time_.load(std::memory_order_acquire);
    if (last_restart > 0 && (now - last_restart) < 15.0) return;
    last_restart_time_.store(now, std::memory_order_release);

    bool expected = false;
    if (!live_restarting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    live_restarting_.store(false, std::memory_order_release);
    PostMessage(hwnd_, WM_RESTART_LIVE, 0, source_generation_.load(std::memory_order_relaxed));
}

void Player::NotifyLiveEof() {
    if (!source_ || !source_->IsLive() || !callback_) return;

    if (live_restarting_.load(std::memory_order_acquire)) return;

    double now = GetTimeSec(perf_freq_);
    double last_frame = last_video_frame_time_.load(std::memory_order_acquire);
    bool long_stall = (last_frame > 0 && (now - last_frame) > 8.0);

    if (!audio_only_) {
        if (!source_->HlsByteStreamHasData()) {
            if (!long_stall) return;
        } else if (!source_->HasNewHlsData()) {
            if (!long_stall) return;
        }
    }

    if (!audio_only_) {
        double last_restart = last_restart_time_.load(std::memory_order_acquire);
        if (last_restart > 0 && (now - last_restart) < 15.0) return;
        last_restart_time_.store(now, std::memory_order_release);
    }

    // Always use FlushAndRestart — never ReopenSource
    LOG_INFO("Live: EOF, immediate restart%s", audio_only_ ? " (audio-only)" : "");
    int gen = source_generation_.load(std::memory_order_relaxed);
    PostMessage(hwnd_, WM_RESTART_LIVE, 0, gen);
}

void Player::FlushAndRestart() {
    if (state_.load(std::memory_order_acquire) != PlayerState::Playing) {
        live_restarting_.store(false, std::memory_order_release);
        return;
    }

    // Prevent concurrent restarts
    bool expected = false;
    if (!live_restarting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    LOG_INFO("Live: flushing + restarting pipeline");
    // Stop callbacks and wait for in-flight to finish
    callback_->Stop();
    callback_->ResetVideoEof();
    callback_->ResetAudioEof();

    // Clear video queue — stale frames cause fast playback after restart
    // But keep the last rendered frame visible (frame_buf_ / frame_ready_)
    // to avoid a black-screen freeze during the 3-4s restart window.
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        for (int i = 0; i < VQ_SIZE; i++) {
            free(vq_[i].data);
            vq_[i].data = nullptr;
            vq_[i].size = 0;
        }
        vq_head_ = 0;
        vq_tail_ = 0;
    }
    // Do NOT set frame_ready_ = false — keep last frame on screen

    // DO NOT reset audio ring buffer — let old audio play out naturally.
    // Resetting would cause audio clock to jump to 0 while video clock
    // continues from old value, creating massive A/V desync.
    // The old buffered audio will drain naturally while new data arrives.

    // Recreate source reader to clear MF's internal EOF state
    IMFSourceReader* new_reader = source_->RecreateReader(callback_);
    if (new_reader) {
        callback_->SetReader(new_reader,
                             source_->GetVideoStream(),
                             source_->GetAudioStream());
    } else {
        LOG_WARN("Live: RecreateReader failed, will retry later");
        live_restarting_.store(false, std::memory_order_release);
        return;
    }

    // DO NOT reset clock — keep old start_time_ so video continues
    // from where it left off. New reader's PTS will align with audio clock.
    // Resetting clock causes video to start from 0 while audio is at old position,
    // creating the "fast-forward" effect as sync rapidly drops frames.
    if (sync_) sync_->Seek();
    // 2s grace period: don't drop frames after restart, let pipeline stabilize
    post_restart_until_.store(GetTimeSec(perf_freq_) + 2.0, std::memory_order_release);
    video_first_frame_post_.store(1, std::memory_order_release);
    callback_->StartReading();

    LOG_INFO("Live: pipeline restarted");
    live_restarting_.store(false, std::memory_order_release);
}

void Player::ReopenSource() {
    if (saved_url_.empty()) {
        LOG_ERROR("ReopenSource: no saved URL");
        return;
    }
    if (state_.load(std::memory_order_acquire) != PlayerState::Playing) return;

    LOG_INFO("ReopenSource: closing and reopening from %S", saved_url_.c_str());

    state_.store(PlayerState::Opening, std::memory_order_release);

    StopVideoTimer();
    if (hwnd_) {
        KillTimer(hwnd_, TIMER_AUDIO_CHECK);
        KillTimer(hwnd_, TIMER_EOF_CHECK);
    }

    if (callback_) callback_->Stop();

    if (ao_) {
        ao_->Pause();
    }

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        for (int i = 0; i < VQ_SIZE; i++) {
            free(vq_[i].data);
            vq_[i].data = nullptr;
            vq_[i].size = 0;
        }
        vq_head_ = 0;
        vq_tail_ = 0;
    }
    frame_ready_ = false;

    if (callback_) callback_->ClearPointers();

    delete vo_;       vo_ = nullptr;
    delete ao_;       ao_ = nullptr;
    delete sync_;     sync_ = nullptr;

    if (source_) { source_->Close(); delete source_; source_ = nullptr; }
    if (callback_) { callback_->Release(); callback_ = nullptr; }

    if (open_thread_.joinable())
        open_thread_.join();

    open_generation_.fetch_add(1, std::memory_order_relaxed);
    source_generation_.fetch_add(1, std::memory_order_relaxed);
    open_running_ = true;
    int gen = open_generation_.load(std::memory_order_relaxed);
    LOG_INFO("ReopenSource: starting open thread (gen=%d)", gen);
    open_thread_ = std::thread([this, gen]() {
        OpenAsync(saved_url_, audio_only_, gen);
    });
}

void Player::ProcessVideoFrame(IMFSample* sample, LONGLONG timestamp) {
    if (!sample) return;
    if (state_.load(std::memory_order_acquire) != PlayerState::Playing) {
        if (callback_) callback_->ConsumeVideo();
        return;
    }

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) {
        if (callback_) callback_->ConsumeVideo();
        return;
    }

    BYTE* data = nullptr;
    DWORD max_len_dw = 0, cur_len_dw = 0;
    if (FAILED(buf->Lock(&data, &max_len_dw, &cur_len_dw))) {
        if (callback_) callback_->ConsumeVideo();
        return;
    }

    const uint8_t* frame_data = data;
    int frame_size = (int)(std::min)(cur_len_dw, (DWORD)INT_MAX);
    uint8_t* converted = nullptr;

    int fw, fh, fs;
    PixelFormat vq_fmt;
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        fw = frame_w_;
        fh = frame_h_;
        fs = stride_;
        vq_fmt = pix_fmt_;
    }

    if (vq_fmt == PixelFormat::NV12 && fh > 0 && cur_len_dw > 0) {
        // NV12 total = stride * actual_height * 3/2
        // MF may report fh with padding (e.g. 1088 for 1080), or the player
        // may have filtered a height change (<16px delta), leaving frame_h_
        // smaller than the actual buffer height.  Search both downward AND
        // upward from fh to find the correct (height, stride) pair.
        fs = 0;
        int found_h = fh;
        // Try exact match first
        {
            size_t nv12_rows = (size_t)fh * 3 / 2;
            if (nv12_rows > 0) {
                size_t s = (size_t)cur_len_dw / nv12_rows;
                if (s >= (size_t)fw && s * nv12_rows == (size_t)cur_len_dw) {
                    fs = (int)s; found_h = fh;
                }
            }
        }
        // Search upward first (known issue: MF reports 1088 but player
        // filters to 1080, yet buffer still has 1088 rows)
        if (fs == 0) {
            for (int h_try = fh + 2; h_try <= fh + 128; h_try += 2) {
                size_t rows2 = (size_t)h_try * 3 / 2;
                if (rows2 == 0) continue;
                size_t s2 = (size_t)cur_len_dw / rows2;
                if (s2 >= (size_t)fw && s2 * rows2 == (size_t)cur_len_dw) {
                    fs = (int)s2; found_h = h_try; break;
                }
            }
        }
        // Search downward (height smaller than reported)
        if (fs == 0) {
            for (int h_try = fh - 2; h_try > 0 && h_try >= fh - 64; h_try -= 2) {
                size_t rows2 = (size_t)h_try * 3 / 2;
                if (rows2 == 0) continue;
                size_t s2 = (size_t)cur_len_dw / rows2;
                if (s2 >= (size_t)fw && s2 * rows2 == (size_t)cur_len_dw) {
                    fs = (int)s2; found_h = h_try; break;
                }
            }
        }
        if (fs == 0) fs = fw;  // ultimate fallback
        fh = found_h;  // use actual buffer height, not MF-filtered height
    }

    if (vq_fmt == PixelFormat::YUY2 && fw > 0 && fh > 0) {
        int nv12_size = (int)(std::min)((size_t)fw * fh * 3 / 2, (size_t)INT_MAX);
        if (convert_buf_size_ < nv12_size) {
            free(convert_buf_);
            convert_buf_ = (uint8_t*)malloc(nv12_size);
            convert_buf_size_ = convert_buf_ ? nv12_size : 0;
        }
        if (convert_buf_ && ::ConvertYUY2ToNV12(data, fw, fh, convert_buf_, nv12_size)) {
            frame_data = convert_buf_; converted = nullptr;
            vq_fmt = PixelFormat::NV12; fs = fw; frame_size = nv12_size;
        } else { LOG_WARN("YUY2→NV12 failed for %dx%d", fw, fh); buf->Unlock(); if (callback_) callback_->ConsumeVideo(); return; }
    } else if (vq_fmt == PixelFormat::I420 && fw > 0 && fh > 0) {
        int nv12_size = (int)(std::min)((size_t)fw * fh * 3 / 2, (size_t)INT_MAX);
        if (convert_buf_size_ < nv12_size) {
            free(convert_buf_);
            convert_buf_ = (uint8_t*)malloc(nv12_size);
            convert_buf_size_ = convert_buf_ ? nv12_size : 0;
        }
        if (convert_buf_ && ::ConvertI420ToNV12(data, fw, fh, convert_buf_, nv12_size)) {
            frame_data = convert_buf_; converted = nullptr;
            vq_fmt = PixelFormat::NV12; fs = fw; frame_size = nv12_size;
        } else { LOG_WARN("I420→NV12 failed for %dx%d", fw, fh); buf->Unlock(); if (callback_) callback_->ConsumeVideo(); return; }
    }

    bool was_empty;
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        was_empty = (vq_head_ == vq_tail_);
        int next_tail = (vq_tail_ + 1) % VQ_SIZE;
        if (next_tail == vq_head_) {
            int old_head = vq_head_;
            vq_[old_head].size = 0;
            vq_head_ = (old_head + 1) % VQ_SIZE;
            if (callback_) callback_->ConsumeVideo();
        }
        bool consumed_in_drop = (next_tail == vq_head_);
        VFrame* f = &vq_[vq_tail_];
        if (!f->data || f->size < frame_size) {
            free(f->data);
            f->data = (uint8_t*)malloc(frame_size);
            if (!f->data) {
                LOG_CRITICAL("Out of memory: ProcessVideoFrame frame buffer");
                if (!consumed_in_drop && callback_) callback_->ConsumeVideo();
                free(converted);
                buf->Unlock();
                return;
            }
        }
        memcpy(f->data, frame_data, frame_size);
        f->size = frame_size;
        f->stride = fs;
        f->width = fw;
        f->height = fh;
        f->timestamp = timestamp / 10000000.0;
        f->pix_fmt = vq_fmt;
        vq_tail_ = next_tail;
    }

    free(converted);
    buf->Unlock();

    last_video_frame_time_.store(GetTimeSec(perf_freq_), std::memory_order_release);

    if (was_empty && video_first_frame_post_.exchange(0, std::memory_order_acq_rel))
        PostMessage(hwnd_, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
}

/*
 * VideoTick — main rendering loop (called from WM_TIMER on main thread).
 */
void Player::VideoTick() {
    if (audio_only_) return;
    if (state_.load(std::memory_order_acquire) != PlayerState::Playing) return;

    // Live HLS: if stream stalled and new data arrived, restart pipeline
    TryRestartLivePipeline();

    // Keep pipeline fed (network: keep at least 15 frames buffered)
    int video_fill_target = is_network_ ? VIDEO_FILL_NETWORK : VIDEO_FILL_LOCAL;
    if (has_video_ && callback_ && !callback_->IsVideoEof()) {
        int qsize;
        {
            std::lock_guard<std::mutex> lock(vq_mutex_);
            qsize = vq_tail_ - vq_head_;
            if (qsize < 0) qsize += VQ_SIZE;
        }
        if (qsize < video_fill_target)
            callback_->RequestVideoRead();
    }

    double audio_clk = 0;
    if (has_audio_ && ao_ && callback_ && !callback_->IsAudioEof())
        audio_clk = ao_->GetClock();
    if (audio_clk <= 0.001)
        audio_clk = ElapsedSec();

    bool rendered = false;

    // During post-restart grace period, force-render all frames instead of dropping
    double now = GetTimeSec(perf_freq_);
    bool in_grace = (now < post_restart_until_.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        int drops_this_tick = 0;
        while (true) {
            int head = vq_head_;
            int tail = vq_tail_;
            if (head == tail) break;

            VFrame* f = &vq_[head];
            SyncDecision sd = sync_->Decide(f->timestamp, audio_clk);
            int next = (head + 1) % VQ_SIZE;
            bool has_more = (next != tail);

            if (sd.action == SyncAction::Drop && has_more && !in_grace && drops_this_tick < 2) {
                free(f->data);
                f->data = nullptr;
                f->size = 0;
                vq_head_ = next;
                callback_->ConsumeVideo();
                drops_this_tick++;
                continue;
            }
            if (sd.action == SyncAction::Wait) {
                break;
            }
            {
                // Move the frame pointer from queue slot to render buffer.
                // This eliminates a full-frame memcpy (~3MB per frame at 1080p).
                // The old frame_buf_ is freed, and the queue slot's data pointer
                // is nulled so it won't be double-freed when the slot is reused.
                free(frame_buf_);
                frame_buf_ = f->data;
                frame_buf_size_ = f->size;
                f->data = nullptr;
                f->size = 0;
                frame_ready_ = true;
                frame_size_ = frame_buf_size_;
                frame_stride_ = f->stride;
                frame_render_w_ = f->width;
                frame_render_h_ = f->height;
                frame_pix_fmt_ = f->pix_fmt;
                if (f->pix_fmt != PixelFormat::Unknown)
                    pix_fmt_ = f->pix_fmt;
            }
            f->size = 0;
            vq_head_ = next;
            callback_->ConsumeVideo();
            rendered = true;
            break;
        }
    }

    if (rendered && win_w_ > 0 && win_h_ > 0)
        if (!RenderD3D(win_w_, win_h_))
            LOG_WARN("RenderD3D: no frame rendered (vo_ uninitialized or no ready frame)");
}

void Player::OnVideoFormatChanged() {
    if (!source_) return;
    source_->ReconfigureVideo();
    VideoInfo vi = source_->GetVideoInfo();
    PixelFormat fmt = source_->GetPixelFormat();
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        if (vi.width > 0) frame_w_ = vi.width;
        if (vi.height > 0) frame_h_ = vi.height;
        if (vi.fps > 0) video_fps_ = vi.fps;
        stride_ = vi.stride;
        pix_fmt_ = fmt;
    }

    // Shrink render buffers if new resolution is significantly smaller
    if (vi.width > 0 && vi.height > 0) {
        int new_size = vi.width * vi.height * 3 / 2;  // NV12 estimate
        if (convert_buf_size_ > new_size * 2) {
            free(convert_buf_);
            convert_buf_ = (uint8_t*)malloc(new_size);
            convert_buf_size_ = convert_buf_ ? new_size : 0;
        }
    }

    LOG_INFO("Player video format updated: %dx%d stride=%d @ %.1f fps",
             frame_w_.load(std::memory_order_relaxed), frame_h_.load(std::memory_order_relaxed),
             stride_.load(std::memory_order_relaxed), video_fps_.load(std::memory_order_relaxed));

    // Restart video timer with updated FPS interval
    StopVideoTimer();
    StartVideoTimer();
}

bool Player::RenderD3D(int w, int h) {
    if (!vo_) return false;
    if (frame_ready_ && frame_buf_ && frame_render_w_ > 0 && frame_render_h_ > 0) {
        vo_->Resize(w, h);
        int stride = frame_stride_;
        if (stride <= 0) stride = frame_render_w_;
        vo_->Render(frame_buf_, frame_render_w_, frame_render_h_, stride, frame_pix_fmt_);
        frame_ready_ = false;
        return true;
    }
    return false;
}

/*
 * CheckAudio — WM_TIMER handler (50 ms interval).
 */
void Player::CheckAudio() {
    if (state_.load(std::memory_order_acquire) != PlayerState::Playing) return;

    // Live HLS: restart if stalled — deduplicated via TryRestartLivePipeline + atomic
    TryRestartLivePipeline();

    // Audio-only HLS: fallback stall detection if NotifyLiveEof didn't trigger
    if (audio_only_ && source_ && source_->IsLive() && callback_ &&
        callback_->IsAudioEof()) {
        int buffered = ao_ ? ao_->GetBuffered() : 0;
        double now = GetTimeSec(perf_freq_);

        if (buffered > 0) {
            last_audio_data_time_.store(now, std::memory_order_release);
        }

        if (buffered == 0) {
            double last_data = last_audio_data_time_.load(std::memory_order_acquire);
            if (last_data > 0 && (now - last_data) >= 1.0) {
                LOG_INFO("Audio-only live: stall fallback (no data for %.1fs)", now - last_data);
                PostMessage(hwnd_, WM_RESTART_LIVE, 0, source_generation_.load(std::memory_order_relaxed));
                last_audio_data_time_.store(now, std::memory_order_release);
            }
        }
    }

    if (has_audio_ && callback_ && !callback_->IsAudioEof()) {
        int buffered = ao_ ? ao_->GetBuffered() : 0;
        int bps = audio_bytes_per_sec_ > 0 ? audio_bytes_per_sec_ : (44100 * 2 * 2);
        int threshold = (std::max)(1, is_network_ ? bps * AUDIO_BUFFER_NETWORK_MULT : bps / AUDIO_BUFFER_LOCAL_DIV);
        if (buffered < threshold) {
            callback_->RequestAudioRead();
        }
    }
}

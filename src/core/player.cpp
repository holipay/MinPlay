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

Player::Player() {
    QueryPerformanceFrequency(&perf_freq_);
}

Player::~Player() {
    Close();
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

bool Player::Open(HWND hwnd, const wchar_t* url) {
    hwnd_ = hwnd;

    callback_ = new (std::nothrow) SourceReaderCallback();
    if (!callback_) {
        LOG_CRITICAL("Out of memory: callback_");
        return false;
    }

    source_ = new (std::nothrow) MediaSource();
    if (!source_) {
        LOG_CRITICAL("Out of memory: source_");
        callback_->Release(); callback_ = nullptr;
        return false;
    }
    if (!source_->Open(url, static_cast<IMFSourceReaderCallback*>(callback_))) {
        LOG_ERROR("Failed to open media source");
        delete source_; source_ = nullptr;
        callback_->Release(); callback_ = nullptr;
        return false;
    }

    has_video_ = source_->HasVideo();
    has_audio_ = source_->HasAudio();
    is_network_ = (wcsncmp(url, L"http://", 7) == 0) ||
                  (wcsncmp(url, L"https://", 8) == 0);
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
        ao_ = new (std::nothrow) WasapiAudioOutput();
        if (!ao_) {
            LOG_CRITICAL("Out of memory: ao_");
            has_audio_ = false;
        } else if (!static_cast<WasapiAudioOutput*>(ao_)->Initialize(
                ai.sample_rate, ai.channels, ai.bits_per_sample)) {
            LOG_ERROR("Failed to initialize audio output");
            delete ao_; ao_ = nullptr;
            has_audio_ = false;
        } else {
            audio_bytes_per_sec_ = ai.sample_rate * ai.channels * (ai.bits_per_sample / 8);
            callback_->SetAudioOutput(ao_);
        }
    }

    double sync_window = SYNC_WINDOW_DEFAULT;
    if (ao_ && ao_->IsExclusive())
        sync_window = SYNC_WINDOW_EXCLUSIVE;
    sync_ = new (std::nothrow) SyncContext(sync_window);
    if (!sync_) {
        LOG_CRITICAL("Out of memory: sync_");
        return false;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    win_w_ = rc.right;
    win_h_ = rc.bottom;

    delete vo_; vo_ = nullptr;
    D3D11VideoOutput* d3d_vo = new (std::nothrow) D3D11VideoOutput();
    if (!d3d_vo) {
        LOG_CRITICAL("Out of memory: d3d_vo");
    } else if (!d3d_vo->Initialize(hwnd, win_w_, win_h_)) {
        LOG_ERROR("Failed to initialize D3D11 video output");
        delete d3d_vo;
    } else {
        vo_ = d3d_vo;
    }

    osd_ = new (std::nothrow) OSD();
    if (!osd_)
        LOG_CRITICAL("Out of memory: osd_");

    callback_->SetPlayer(this);
    callback_->SetReader(source_->GetReader(),
                          source_->GetVideoStream(),
                          source_->GetAudioStream());
    return true;
}

void Player::Close() {
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
    frame_size_ = 0;
    pix_fmt_.store(PixelFormat::Unknown, std::memory_order_relaxed);
}

void Player::Play() {
    if (!source_) return;
    if (state_.load(std::memory_order_acquire) != PlayerState::Stopped) return;

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

void Player::PauseToggle() {
    if (state_.load(std::memory_order_acquire) == PlayerState::Playing) {
        state_.store(PlayerState::Paused, std::memory_order_release);
        pause_start_.store(GetTimeSec(perf_freq_), std::memory_order_release);
        if (callback_) callback_->Stop();
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
    bool was_playing = (state_.load(std::memory_order_acquire) == PlayerState::Playing);
    bool was_paused  = (state_.load(std::memory_order_acquire) == PlayerState::Paused);

    if (callback_) callback_->Stop();
    if (ao_) ao_->Reset();
    StopVideoTimer();

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        vq_head_ = 0;
        vq_tail_ = 0;
    }

    source_->Seek(seconds);

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame_ready_ = false;
    }

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

void Player::Resize(int w, int h) {
    win_w_ = w;
    win_h_ = h;
}

void Player::Paint(HDC hdc, int /*w*/, int /*h*/) {
    if (osd_) {
        osd_->Draw(hdc, GetPosition(), GetDuration(),
                   (int)video_fps_.load(std::memory_order_relaxed));
    }
    PostMessage(hwnd_, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
}

bool Player::IsFinished() const {
    if (source_ && source_->IsLive()) return false;
    bool vq_empty;
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        vq_empty = (vq_head_ == vq_tail_);
    }
    bool f_ready;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        f_ready = frame_ready_;
    }
    bool vdone  = !has_video_ || (callback_ && callback_->IsVideoEof() && vq_empty && !f_ready);
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
    if (!source_ || !source_->IsLive() || !callback_) return;
    if (!source_->HlsByteStreamHasData() || !source_->HasNewHlsData()) return;
    if (!callback_->IsVideoEof() && !callback_->IsAudioEof()) return;

    // Prevent concurrent restart from both VideoTick and CheckAudio
    bool expected = false;
    if (!live_restarting_.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    // Defer actual Flush + restart to message loop — avoids blocking WM_TIMER
    // with MF internal locks held during Flush() calls.
    PostMessage(hwnd_, WM_RESTART_LIVE, 0, 0);
}

void Player::FlushAndRestart() {
    LOG_INFO("Live: new HLS data, flushing + restarting pipeline");
    if (source_->GetReader()) {
        IMFSourceReader* r = source_->GetReader();
        r->Flush(source_->GetVideoStream());
        r->Flush(source_->GetAudioStream());
        callback_->ResetVideoEof();
        callback_->ResetAudioEof();
        // Reset clock so new frame timestamps (~0) match the wall clock
        double now = GetTimeSec(perf_freq_);
        start_time_.store(now, std::memory_order_release);
        pause_offset_.store(0, std::memory_order_release);
        pause_start_.store(0, std::memory_order_release);
        if (sync_) sync_->Seek();
        callback_->RequestVideoRead();
        callback_->RequestAudioRead();
    }

    live_restarting_.store(false, std::memory_order_release);
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
        size_t nv12_pitch = (size_t)fh * 3 / 2;
        int actual = (int)((size_t)cur_len_dw / nv12_pitch);
        if (actual >= fw) fs = actual;
    }

    if (vq_fmt == PixelFormat::YUY2 && fw > 0 && fh > 0) {
        converted = ConvertYUY2ToNV12(data, fw, fh);
        if (converted) { frame_data = converted; vq_fmt = PixelFormat::NV12; fs = fw; frame_size = (int)(std::min)((size_t)fw * fh * 3 / 2, (size_t)INT_MAX); }
        else { LOG_WARN("YUY2→NV12 OOM for %dx%d", fw, fh); buf->Unlock(); if (callback_) callback_->ConsumeVideo(); return; }
    } else if (vq_fmt == PixelFormat::I420 && fw > 0 && fh > 0) {
        converted = ConvertI420ToNV12(data, fw, fh);
        if (converted) { frame_data = converted; vq_fmt = PixelFormat::NV12; fs = fw; frame_size = (int)(std::min)((size_t)fw * fh * 3 / 2, (size_t)INT_MAX); }
        else { LOG_WARN("I420→NV12 OOM for %dx%d", fw, fh); buf->Unlock(); if (callback_) callback_->ConsumeVideo(); return; }
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

    if (was_empty && video_first_frame_post_.exchange(0, std::memory_order_acq_rel))
        PostMessage(hwnd_, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
}

/*
 * VideoTick — main rendering loop (called from WM_TIMER on main thread).
 */
void Player::VideoTick() {
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

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        while (true) {
            int head = vq_head_;
            int tail = vq_tail_;
            if (head == tail) break;

            VFrame* f = &vq_[head];
            SyncDecision sd = sync_->Decide(f->timestamp, audio_clk);
            int next = (head + 1) % VQ_SIZE;
            bool has_more = (next != tail);

            if (sd.action == SyncAction::Drop && has_more) {
                f->size = 0;
                vq_head_ = next;
                callback_->ConsumeVideo();
                continue;
            }
            if (sd.action == SyncAction::Wait) {
                break;
            }
            {
                std::lock_guard<std::mutex> lock_f(frame_mutex_);
                if (f->size > frame_buf_size_) {
                    uint8_t* nb = (uint8_t*)realloc(frame_buf_, f->size);
                    if (nb) {
                        frame_buf_ = nb;
                        frame_buf_size_ = f->size;
                    } else {
                        LOG_WARN("OOM realloc frame_buf_: %d bytes", f->size);
                        // Leave frame in queue — retry next tick
                        break;
                    }
                }
                if (frame_buf_ && f->size <= frame_buf_size_) {
                    memcpy(frame_buf_, f->data, f->size);
                    frame_ready_ = true;
                    frame_size_ = f->size;
                    frame_stride_ = f->stride;
                    frame_render_w_ = f->width;
                    frame_render_h_ = f->height;
                    frame_pix_fmt_ = f->pix_fmt;
                    if (f->pix_fmt != PixelFormat::Unknown)
                        pix_fmt_ = f->pix_fmt;
                }
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
    LOG_INFO("Player video format updated: %dx%d stride=%d @ %.1f fps",
             frame_w_.load(std::memory_order_relaxed), frame_h_.load(std::memory_order_relaxed),
             stride_.load(std::memory_order_relaxed), video_fps_.load(std::memory_order_relaxed));
}

bool Player::RenderD3D(int w, int h) {
    if (!vo_) return false;
    std::lock_guard<std::mutex> lock(frame_mutex_);
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

    if (has_audio_ && callback_ && !callback_->IsAudioEof()) {
        int buffered = ao_ ? ao_->GetBuffered() : 0;
        int bps = audio_bytes_per_sec_ > 0 ? audio_bytes_per_sec_ : (44100 * 2 * 2);
        int threshold = (std::max)(1, is_network_ ? bps * AUDIO_BUFFER_NETWORK_MULT : bps / AUDIO_BUFFER_LOCAL_DIV);
        if (buffered < threshold) {
            callback_->RequestAudioRead();
        }
    }
}

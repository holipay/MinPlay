#include "player.h"
#include "../media/media_source.h"
#include "../video_out/d3d11_video_output.h"
#include "../audio_out/wasapi_audio_output.h"
#include "../util/log.h"
#include <cstdlib>
#include <cstring>
#include <cmath>

Player::Player() {
    QueryPerformanceFrequency(&perf_freq_);
    frame_buf_size_ = 3840 * 2160 * 4;
    frame_buf_ = (uint8_t*)malloc(frame_buf_size_);
}

Player::~Player() {
    Destroy();
}

double Player::GetTimeSec(const LARGE_INTEGER& freq) {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (double)li.QuadPart / freq.QuadPart;
}

double Player::ElapsedSec() const {
    LARGE_INTEGER freq = perf_freq_;
    return GetTimeSec(freq) - start_time_ - pause_offset_;
}

bool Player::Open(HWND hwnd, const wchar_t* url) {
    hwnd_ = hwnd;

    callback_ = new SourceReaderCallback();
    if (!callback_) return false;

    source_ = new MediaSource();
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
        video_fps_ = vi.fps;
    }

    if (has_audio_) {
        AudioInfo ai = source_->GetAudioInfo();
        ao_ = new WasapiAudioOutput();
        if (!static_cast<WasapiAudioOutput*>(ao_)->Initialize(
                ai.sample_rate, ai.channels, ai.bits_per_sample)) {
            LOG_ERROR("Failed to initialize audio output");
            delete ao_; ao_ = nullptr;
            has_audio_ = false;
        } else {
            audio_bytes_per_sec_ = ai.sample_rate * ai.channels * (ai.bits_per_sample / 8);
            callback_->SetAudioOutput(ao_);
        }
    }

    double sync_window = 0.020;
    if (ao_ && ao_->IsExclusive())
        sync_window = 0.010;
    sync_ = new SyncContext(sync_window);

    RECT rc;
    GetClientRect(hwnd, &rc);
    win_w_ = rc.right;
    win_h_ = rc.bottom;

    D3D11VideoOutput* d3d_vo = new D3D11VideoOutput();
    if (!d3d_vo->Initialize(hwnd, win_w_, win_h_)) {
        LOG_ERROR("Failed to initialize D3D11 video output");
        delete d3d_vo;
    } else {
        vo_ = d3d_vo;
    }

    osd_ = new OSD();

    callback_->SetPlayer(this);
    callback_->SetReader(source_->GetReader(),
                          source_->GetVideoStream(),
                          source_->GetAudioStream());
    return true;
}

void Player::Close() {
    if (callback_) callback_->Stop();

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        for (int i = 0; i < VQ_SIZE; i++) {
            free(vq_[i].data);
            vq_[i].data = nullptr;
        }
        vq_head_ = 0;
        vq_tail_ = 0;
    }

    delete vo_;       vo_ = nullptr;
    delete ao_;       ao_ = nullptr;
    delete sync_;     sync_ = nullptr;
    delete osd_;      osd_ = nullptr;

    if (source_) { source_->Close(); delete source_; source_ = nullptr; }
    if (callback_) { callback_->Release(); callback_ = nullptr; }
}

void Player::Destroy() {
    Close();
    free(frame_buf_);
    frame_buf_ = nullptr;
}

void Player::Play() {
    if (!source_) return;
    state_ = PlayerState::Playing;
    start_time_ = GetTimeSec(perf_freq_);
    pause_offset_ = 0;

    InterlockedExchange(&video_first_frame_post_, 1);
    if (ao_) ao_->Resume();
    if (callback_) callback_->StartReading();
}

void Player::PauseToggle() {
    if (state_ == PlayerState::Playing) {
        state_ = PlayerState::Paused;
        pause_start_ = GetTimeSec(perf_freq_);
        if (callback_) callback_->Stop();
        if (ao_) ao_->Pause();
    } else if (state_ == PlayerState::Paused) {
        state_ = PlayerState::Playing;
        pause_offset_ += GetTimeSec(perf_freq_) - pause_start_;
        if (ao_) ao_->Resume();
        if (callback_) callback_->StartReading();
    }
}

void Player::Seek(double seconds) {
    if (!source_) return;
    if (source_->IsLive()) return;
    if (seconds < 0) seconds = 0;
    bool was_playing = (state_ == PlayerState::Playing);

    if (callback_) callback_->Stop();
    if (ao_) ao_->Reset();

    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        for (int i = 0; i < VQ_SIZE; i++) {
            free(vq_[i].data);
            vq_[i].data = nullptr;
        }
        vq_head_ = 0;
        vq_tail_ = 0;
    }

    source_->Seek(seconds);

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame_ready_ = false;
    }

    if (was_playing) {
        state_ = PlayerState::Playing;
        start_time_ = GetTimeSec(perf_freq_) - seconds;
        pause_offset_ = 0;
        InterlockedExchange(&video_first_frame_post_, 1);
        if (ao_) ao_->Resume();
        if (callback_) callback_->StartReading();
    }
}

double Player::GetDuration() const {
    if (source_ && source_->IsLive()) return -1;
    return source_ ? source_->Duration() : 0;
}

double Player::GetPosition() const {
    double pos = ElapsedSec();
    double dur = GetDuration();
    if (pos > dur) pos = dur;
    return pos < 0 ? 0 : pos;
}

void Player::Resize(int w, int h) {
    win_w_ = w;
    win_h_ = h;
}

void Player::Paint(HDC /*hdc*/, int /*w*/, int /*h*/) {
    // D3D11 rendering — WM_PAINT is a no-op
}

bool Player::IsFinished() const {
    if (source_ && source_->IsLive()) return false;
    bool vq_empty = (vq_head_ == vq_tail_);
    bool vdone  = !has_video_ || (callback_ && callback_->IsVideoEof() && vq_empty && !frame_ready_);
    bool adone  = !has_audio_ || (callback_ && callback_->IsAudioEof() && ao_ && ao_->GetBuffered() == 0);
    return vdone && adone;
}

uint8_t* Player::ConvertYUY2ToNV12(const uint8_t* yuy2, int w, int h) {
    int y_size = w * h;
    int nv12_size = y_size + y_size / 2;
    uint8_t* nv12 = (uint8_t*)malloc(nv12_size);
    if (!nv12) return nullptr;

    uint8_t* yp = nv12;
    uint8_t* uvp = nv12 + y_size;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col += 2) {
            int src_idx = (row * w + col) * 2;
            yp[row * w + col]     = yuy2[src_idx];
            yp[row * w + col + 1] = yuy2[src_idx + 2];
            if ((row & 1) == 0) {
                int uv_idx = (row / 2) * w + col;
                uvp[uv_idx]     = yuy2[src_idx + 1];
                uvp[uv_idx + 1] = yuy2[src_idx + 3];
            }
        }
    }
    return nv12;
}

uint8_t* Player::ConvertI420ToNV12(const uint8_t* i420, int w, int h) {
    int y_size = w * h;
    int nv12_size = y_size + y_size / 2;
    uint8_t* nv12 = (uint8_t*)malloc(nv12_size);
    if (!nv12) return nullptr;

    memcpy(nv12, i420, y_size);
    uint8_t* uvp = nv12 + y_size;
    const uint8_t* up = i420 + y_size;
    const uint8_t* vp = up + (w / 2) * (h / 2);

    for (int row = 0; row < h / 2; row++) {
        for (int col = 0; col < w / 2; col++) {
            uvp[row * w + col * 2]     = up[row * (w / 2) + col];
            uvp[row * w + col * 2 + 1] = vp[row * (w / 2) + col];
        }
    }
    return nv12;
}

void Player::ProcessVideoFrame(IMFSample* sample, LONGLONG timestamp) {
    if (!sample || state_ != PlayerState::Playing) return;

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;

    BYTE* data = nullptr;
    DWORD max_len = 0, cur_len = 0;
    if (FAILED(buf->Lock(&data, &max_len, &cur_len))) return;

    const uint8_t* frame_data = data;
    int frame_size = (int)cur_len;
    uint8_t* converted = nullptr;

    // Read frame dimensions + format under vq_mutex_ (same lock as VideoTick)
    int fw, fh;
    PixelFormat vq_fmt;
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        fw = frame_w_;
        fh = frame_h_;
        vq_fmt = pix_fmt_;
    }

    if (vq_fmt == PixelFormat::YUY2 && fw > 0 && fh > 0) {
        converted = ConvertYUY2ToNV12(data, fw, fh);
        if (converted) { frame_data = converted; vq_fmt = PixelFormat::NV12; frame_size = fw * fh * 3 / 2; }
    } else if (vq_fmt == PixelFormat::I420 && fw > 0 && fh > 0) {
        converted = ConvertI420ToNV12(data, fw, fh);
        if (converted) { frame_data = converted; vq_fmt = PixelFormat::NV12; frame_size = fw * fh * 3 / 2; }
    }

    bool was_empty;
    {
        std::lock_guard<std::mutex> lock(vq_mutex_);
        was_empty = (vq_head_ == vq_tail_);
        int next_tail = (vq_tail_ + 1) % VQ_SIZE;
        if (next_tail == vq_head_) {
            int old_head = vq_head_;
            free(vq_[old_head].data);
            vq_[old_head].data = nullptr;
            vq_head_ = (old_head + 1) % VQ_SIZE;
            if (callback_) callback_->ConsumeVideo();
        }
        VFrame* f = &vq_[vq_tail_];
        f->data = (uint8_t*)malloc(frame_size);
        memcpy(f->data, frame_data, frame_size);
        f->size = frame_size;
        f->timestamp = timestamp / 10000000.0;
        f->pix_fmt = vq_fmt;
        vq_tail_ = next_tail;
    }

    free(converted);
    buf->Unlock();

    if (was_empty && InterlockedExchange(&video_first_frame_post_, 0))
        PostMessage(hwnd_, WM_TIMER, TIMER_VIDEO_DISPLAY, 0);
}

void Player::VideoTick() {
    if (!this || state_ != PlayerState::Playing) return;

    // Live HLS: if a stream stalled and new data arrived, restart the pipeline
    if (source_ && source_->IsLive() &&
        callback_ && (callback_->IsVideoEof() || callback_->IsAudioEof()) &&
        source_->HlsByteStreamHasData() && source_->HasNewHlsData()) {
        LOG_INFO("Live: new HLS data, flushing + restarting pipeline");
        if (source_->GetReader()) {
            IMFSourceReader* r = source_->GetReader();
            r->Flush(source_->GetVideoStream());
            r->Flush(source_->GetAudioStream());
            callback_->ResetVideoEof();
            callback_->ResetAudioEof();
            // Reset clock so new frame timestamps (~0) match the wall clock
            start_time_ = GetTimeSec(perf_freq_);
            pause_offset_ = 0;
            if (sync_) sync_->Seek();
        }
    }

    // Keep pipeline fed (network: keep at least 15 frames buffered)
    int video_fill_target = is_network_ ? 15 : 1;
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

    // Early-out if no frames
    if (vq_head_ == vq_tail_ && !frame_ready_)
        return;

    // Get audio clock — after audio EOF the ring-buffer clock estimate freezes
    // (last_write_pts_ stays at the final sample while RingAvail drains to 0),
    // so fall back to wall-clock elapsed time instead.
    double audio_clk = 0;
    if (has_audio_ && ao_ && callback_ && !callback_->IsAudioEof())
        audio_clk = ao_->GetClock();
    if (audio_clk <= 0.001)
        audio_clk = ElapsedSec();

    // Find best frame to render using sync
    VFrame frame_to_render{};
    bool have_frame = false;

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
                free(f->data); f->data = nullptr;
                vq_head_ = next;
                callback_->ConsumeVideo();
                continue;
            }
            if (sd.action == SyncAction::Wait) {
                break;
            }
            // SYNC_RENDER
            frame_to_render.data = (uint8_t*)malloc(f->size);
            memcpy(frame_to_render.data, f->data, f->size);
            frame_to_render.size = f->size;
            frame_to_render.timestamp = f->timestamp;
            frame_to_render.pix_fmt = f->pix_fmt;
            have_frame = true;

            free(f->data); f->data = nullptr;
            vq_head_ = next;
            callback_->ConsumeVideo();
            break;
        }
    }

    if (have_frame) {
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            if (frame_buf_ && frame_to_render.size <= frame_buf_size_) {
                memcpy(frame_buf_, frame_to_render.data, frame_to_render.size);
                frame_ready_ = true;
                frame_size_ = frame_to_render.size;



                // Update pixel format from frame
                if (frame_to_render.pix_fmt != PixelFormat::Unknown)
                    pix_fmt_ = frame_to_render.pix_fmt;
            }
        }
        // ~VFrame dtor will free(frame_to_render.data)
    }

    // Refill pipeline
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

    // Render
    if (win_w_ > 0 && win_h_ > 0)
        RenderD3D(win_w_, win_h_);
}

void Player::OnVideoFormatChanged() {
    if (!source_) return;
    std::lock_guard<std::mutex> lock(vq_mutex_);
    source_->ReconfigureVideo();
    VideoInfo vi = source_->GetVideoInfo();
    if (vi.width > 0) frame_w_ = vi.width;
    if (vi.height > 0) frame_h_ = vi.height;
    if (vi.fps > 0) video_fps_ = vi.fps;
    pix_fmt_ = source_->GetPixelFormat();
    LOG_INFO("Player video format updated: %dx%d @ %.1f fps",
             frame_w_, frame_h_, video_fps_);
}

void Player::RenderD3D(int w, int h) {
    if (!vo_) return;
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frame_ready_ && frame_buf_ && frame_w_ > 0 && frame_h_ > 0) {
        vo_->Resize(w, h);
        vo_->Render(frame_buf_, frame_w_, frame_h_, frame_size_);
        frame_ready_ = false;
    }
}

void Player::CheckAudio() {
    if (!this || state_ != PlayerState::Playing) return;

    if (has_audio_ && callback_ && !callback_->IsAudioEof()) {
        int buffered = ao_ ? ao_->GetBuffered() : 0;
        int bps = audio_bytes_per_sec_ > 0 ? audio_bytes_per_sec_ : (44100 * 2 * 2);
        int threshold = is_network_ ? bps * 5 : bps / 5;
        if (buffered < threshold) {
            callback_->RequestAudioRead();
        }
    }
}

#pragma once

enum class SyncAction { Render, Wait, Drop };

struct SyncDecision {
    SyncAction action = SyncAction::Render;
    int wait_ms = 0;
    double diff_ms = 0;
};

/*
 * A/V sync via frame drop/delay:
 *   sync_window_  — frames within ±this range of audio clock are rendered immediately
 *   drop_threshold_ — frames more than 5× sync_window behind audio are dropped
 *   wait_limit_   — frames ahead of audio wait at most this long (200 ms)
 *
 * No audio speed adjustment: video-only correction.
 */
class SyncContext {
public:
    explicit SyncContext(double sync_window_sec);
    ~SyncContext() = default;

    void SetWindow(double window_sec);
    SyncDecision Decide(double video_pts, double audio_clk);
    void Seek();

private:
    double sync_window_;
    double drop_threshold_;
    double wait_limit_;
    int stat_drops_ = 0;
    int stat_frames_ = 0;
};

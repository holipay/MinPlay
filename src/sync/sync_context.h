#pragma once

enum class SyncAction { Render, Wait, Drop };

struct SyncDecision {
    SyncAction action = SyncAction::Render;
    int wait_ms = 0;
    double diff_ms = 0;
};

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

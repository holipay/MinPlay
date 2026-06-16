#include "sync_context.h"
#include <cmath>
#include <cstdlib>

SyncContext::SyncContext(double sync_window_sec)
    : sync_window_(sync_window_sec)
    , drop_threshold_(sync_window_sec * 5)
    , wait_limit_(0.200)
{}

void SyncContext::SetWindow(double window_sec) {
    sync_window_ = window_sec;
    drop_threshold_ = window_sec * 5;
}

SyncDecision SyncContext::Decide(double video_pts, double audio_clk) {
    SyncDecision d;
    double diff = video_pts - audio_clk;
    d.diff_ms = diff * 1000.0;
    stat_frames_++;

    if (diff < -drop_threshold_) {
        d.action = SyncAction::Drop;
        stat_drops_++;
    } else if (diff < -sync_window_) {
        d.action = SyncAction::Render;
    } else if (diff < sync_window_) {
        d.action = SyncAction::Render;
    } else if (diff < wait_limit_) {
        d.action = SyncAction::Wait;
        d.wait_ms = (int)(diff * 1000.0);
        if (d.wait_ms < 1) d.wait_ms = 1;
    } else {
        d.action = SyncAction::Wait;
        d.wait_ms = (int)(wait_limit_ * 1000.0);
    }
    return d;
}

void SyncContext::Seek() {
    stat_drops_ = 0;
    stat_frames_ = 0;
}

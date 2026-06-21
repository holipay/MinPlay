#include "sync_context.h"
#include <cmath>
#include <cstdlib>

/*
 * Sync window regions (diff = video_pts - audio_clk in seconds):
 *   diff < -drop_threshold_  → frame is too late, DROP
 *   diff < -sync_window_     → slightly late, render immediately (catch-up)
 *   |diff| ≤ sync_window_    → within tolerance, render immediately
 *   diff < wait_limit_       → ahead, WAIT for audio to catch up
 *   diff ≥ wait_limit_       → far ahead, WAIT clamped to wait_limit_
 */
SyncContext::SyncContext(double sync_window_sec)
    : sync_window_(sync_window_sec)
    , drop_threshold_(2.0)  // 2 seconds — network streams buffer heavily, allow large drift
    , wait_limit_(0.500)
{}

void SyncContext::SetWindow(double window_sec) {
    sync_window_ = window_sec;
    // drop_threshold_ stays at 2.0 — intentionally large for network buffering
}

SyncDecision SyncContext::Decide(double video_pts, double audio_clk) {
    SyncDecision d;
    double diff = video_pts - audio_clk;
    d.diff_ms = diff * 1000.0;
    stat_frames_.fetch_add(1, std::memory_order_relaxed);

    if (diff < -drop_threshold_) {
        d.action = SyncAction::Drop;
        stat_drops_.fetch_add(1, std::memory_order_relaxed);
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
    stat_drops_.store(0, std::memory_order_relaxed);
    stat_frames_.store(0, std::memory_order_relaxed);
}

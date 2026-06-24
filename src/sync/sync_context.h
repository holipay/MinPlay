#pragma once
#include <atomic>

enum class SyncAction { Render, Wait, Drop };

struct SyncDecision {
    SyncAction action = SyncAction::Render;
    int wait_ms = 0;
    double diff_ms = 0;
};

/*
 * A/V sync via frame drop/delay:
 *   sync_window_  — frames within ±this range of audio clock are rendered immediately
 *   drop_threshold_ — frames more than this far behind audio are dropped (150 ms)
 *   wait_limit_   — frames ahead of audio wait at most this long (500 ms)
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

    int GetDrops() const { return stat_drops_.load(std::memory_order_relaxed); }
    int GetFrames() const { return stat_frames_.load(std::memory_order_relaxed); }
    void ResetStats() { stat_drops_.store(0, std::memory_order_relaxed); stat_frames_.store(0, std::memory_order_relaxed); }

private:
    double sync_window_;
    double drop_threshold_;
    double wait_limit_;
    // Stats counters — accessed from VideoTick and CheckAudio (both main thread,
    // sequential, so no real race; atomic for future-proofing)
    std::atomic<int> stat_drops_{0};
    std::atomic<int> stat_frames_{0};
};

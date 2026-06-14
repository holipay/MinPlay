#include "sync.h"
#include "sync_debug.h"
#include <stdlib.h>
#include <math.h>

struct SyncContext {
    double sync_window;
    double drop_threshold;
    double wait_limit;
    int    stat_drops;
    int    stat_frames;
};

SyncContext* sync_create(double sync_window_sec) {
    SyncContext* ctx = (SyncContext*)calloc(1, sizeof(SyncContext));
    ctx->sync_window    = sync_window_sec;
    ctx->drop_threshold = sync_window_sec * 5;
    ctx->wait_limit     = 0.200;
    return ctx;
}

void sync_set_window(SyncContext* ctx, double window_sec) {
    if (!ctx) return;
    ctx->sync_window    = window_sec;
    ctx->drop_threshold = window_sec * 5;
}

SyncDecision sync_video(SyncContext* ctx, double video_pts, double audio_clk) {
    SyncDecision d = {0};
    double diff = video_pts - audio_clk;
    d.diff_ms = diff * 1000.0;
    ctx->stat_frames++;

    sync_debug_log(video_pts, audio_clk, d.diff_ms);

    if (diff < -ctx->drop_threshold) {
        d.action = SYNC_DROP;
        ctx->stat_drops++;
    } else if (diff < -ctx->sync_window) {
        d.action = SYNC_RENDER;
    } else if (diff < ctx->sync_window) {
        d.action = SYNC_RENDER;
    } else if (diff < ctx->wait_limit) {
        d.action = SYNC_WAIT;
        d.wait_ms = (int)(diff * 1000.0);
    } else {
        d.action = SYNC_WAIT;
        d.wait_ms = (int)(ctx->wait_limit * 1000.0);
    }
    return d;
}

void sync_seek(SyncContext* ctx) {
    ctx->stat_drops = 0;
    ctx->stat_frames = 0;
}

void sync_destroy(SyncContext* ctx) {
    if (!ctx) return;
    sync_debug_close();
    free(ctx);
}

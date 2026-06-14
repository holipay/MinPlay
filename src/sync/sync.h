#ifndef SYNC_H
#define SYNC_H

typedef enum {
    SYNC_RENDER,
    SYNC_WAIT,
    SYNC_DROP
} SyncAction;

typedef struct {
    SyncAction action;
    int        wait_ms;
    double     diff_ms;
} SyncDecision;

typedef struct SyncContext SyncContext;

SyncContext* sync_create(double sync_window_sec);
void         sync_set_window(SyncContext* ctx, double window_sec);
SyncDecision sync_video(SyncContext* ctx, double video_pts, double audio_clk);
void         sync_seek(SyncContext* ctx);
void         sync_destroy(SyncContext* ctx);

#endif

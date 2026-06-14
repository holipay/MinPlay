#ifndef SYNC_DEBUG_H
#define SYNC_DEBUG_H

#ifdef SYNC_DEBUG_ENABLED
void sync_debug_open(const char* filename);
void sync_debug_log(double video_pts, double audio_clk, double diff_ms);
void sync_debug_close(void);
#else
#define sync_debug_open(f)      ((void)0)
#define sync_debug_log(v,a,d)   ((void)0)
#define sync_debug_close()      ((void)0)
#endif

#endif

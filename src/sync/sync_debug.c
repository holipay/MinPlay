#ifdef SYNC_DEBUG_ENABLED

#include "sync_debug.h"
#include <windows.h>
#include <stdio.h>

static FILE* g_log = NULL;
static LARGE_INTEGER g_freq, g_start;
static int g_init = 0;

void sync_debug_open(const char* filename) {
    g_log = fopen(filename, "w");
    if (g_log) {
        fprintf(g_log, "wall_time,video_pts,audio_clk,diff_ms,action\n");
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_start);
        g_init = 1;
    }
}

void sync_debug_log(double video_pts, double audio_clk, double diff_ms) {
    if (!g_log || !g_init) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double wall = (double)(now.QuadPart - g_start.QuadPart) / g_freq.QuadPart;
    const char* action = "OK";
    if (diff_ms < -100) action = "DROP";
    else if (diff_ms < -10) action = "CHASE";
    else if (diff_ms > 10) action = "WAIT";
    fprintf(g_log, "%.4f,%.4f,%.4f,%.1f,%s\n", wall, video_pts, audio_clk, diff_ms, action);
    static int count = 0;
    if (++count % 100 == 0) fflush(g_log);
}

void sync_debug_close(void) {
    if (g_log) { fclose(g_log); g_log = NULL; }
}

#endif

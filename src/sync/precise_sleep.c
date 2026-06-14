#include "precise_sleep.h"
#include <windows.h>
#include <stdint.h>

static LARGE_INTEGER g_freq;
static int g_initialized = 0;

void precise_sleep_init(void) {
    if (g_initialized) return;
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);
    g_initialized = 1;
}

void precise_sleep(double seconds) {
    if (!g_initialized) precise_sleep_init();
    if (seconds <= 0.0) return;

    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    int64_t target_ticks = (int64_t)(seconds * g_freq.QuadPart);

    int sleep_ms = (int)(seconds * 1000.0) - 2;
    if (sleep_ms > 0) Sleep(sleep_ms);

    do {
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - start.QuadPart) < target_ticks);
}

void precise_sleep_cleanup(void) {
    if (g_initialized) {
        timeEndPeriod(1);
        g_initialized = 0;
    }
}

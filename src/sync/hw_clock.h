#ifndef HW_CLOCK_H
#define HW_CLOCK_H

#include <stdint.h>

typedef struct HWClock HWClock;

HWClock*    hwclock_create(int sample_rate, int channels, int bits, int exclusive);
int         hwclock_write(HWClock* clk, const uint8_t* pcm, int size, double pts);
double      hwclock_get_time(HWClock* clk);
void        hwclock_reset(HWClock* clk, double new_pts);
void        hwclock_flush(HWClock* clk);
double      hwclock_get_latency(HWClock* clk);
void        hwclock_destroy(HWClock* clk);

#endif

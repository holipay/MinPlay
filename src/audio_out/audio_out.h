#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>

typedef struct AudioOut AudioOut;

AudioOut*   ao_create(int sample_rate, int channels, int bits);
void        ao_write(AudioOut* ao, const uint8_t* data, int size);
void        ao_set_pts(AudioOut* ao, double pts);
int         ao_get_buffered(AudioOut* ao);
int         ao_get_free(AudioOut* ao);
int         ao_is_playing(AudioOut* ao);
double      ao_get_position_sec(AudioOut* ao);
double      ao_get_clock(AudioOut* ao);
void        ao_set_speed(AudioOut* ao, double speed);
void        ao_pause(AudioOut* ao);
void        ao_resume(AudioOut* ao);
void        ao_reset(AudioOut* ao);
int         ao_is_exclusive(AudioOut* ao);
void        ao_destroy(AudioOut* ao);

#endif

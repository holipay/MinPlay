#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

#include <stdint.h>

typedef struct AudioOut AudioOut;

AudioOut*   ao_create(int sample_rate, int channels, int bits);
void        ao_write(AudioOut* ao, const uint8_t* data, int size);
void        ao_pause(AudioOut* ao);
void        ao_resume(AudioOut* ao);
void        ao_reset(AudioOut* ao);
void        ao_destroy(AudioOut* ao);

#endif

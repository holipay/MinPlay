#ifndef VIDEO_OUT_H
#define VIDEO_OUT_H

#include <windows.h>
#include <stdint.h>

typedef struct VideoOut VideoOut;

VideoOut*   vo_create(HWND hwnd, int width, int height);
void        vo_render(VideoOut* vo, const uint8_t* data,
                       int src_w, int src_h, int data_size);
void        vo_resize(VideoOut* vo, int win_w, int win_h);
void        vo_destroy(VideoOut* vo);

#endif

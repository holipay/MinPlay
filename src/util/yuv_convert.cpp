#include "yuv_convert.h"
#include <cstdlib>
#include <cstring>

uint8_t* ConvertYUY2ToNV12(const uint8_t* yuy2, int w, int h) {
    w &= ~1;
    if (w < 2) return nullptr;
    size_t y_size = (size_t)w * h;
    size_t nv12_size = y_size + y_size / 2;
    uint8_t* nv12 = (uint8_t*)malloc(nv12_size);
    if (!nv12) return nullptr;

    uint8_t* yp = nv12;
    uint8_t* uvp = nv12 + y_size;
    size_t wu = (size_t)w;
    size_t hu = (size_t)h;
    for (size_t row = 0; row < hu; row++) {
        for (size_t col = 0; col < wu; col += 2) {
            size_t src_idx = (row * wu + col) * 2;
            yp[row * wu + col]     = yuy2[src_idx];
            yp[row * wu + col + 1] = yuy2[src_idx + 2];
            if ((row & 1) == 0) {
                size_t uv_idx = (row / 2) * wu + col;
                uvp[uv_idx]     = yuy2[src_idx + 1];
                uvp[uv_idx + 1] = yuy2[src_idx + 3];
            }
        }
    }
    return nv12;
}

bool ConvertYUY2ToNV12(const uint8_t* yuy2, int w, int h, uint8_t* out, int out_size) {
    w &= ~1;
    if (w < 2) return false;
    size_t y_size = (size_t)w * h;
    size_t nv12_size = y_size + y_size / 2;
    if ((size_t)out_size < nv12_size) return false;

    uint8_t* yp = out;
    uint8_t* uvp = out + y_size;
    size_t wu = (size_t)w;
    size_t hu = (size_t)h;
    for (size_t row = 0; row < hu; row++) {
        for (size_t col = 0; col < wu; col += 2) {
            size_t src_idx = (row * wu + col) * 2;
            yp[row * wu + col]     = yuy2[src_idx];
            yp[row * wu + col + 1] = yuy2[src_idx + 2];
            if ((row & 1) == 0) {
                size_t uv_idx = (row / 2) * wu + col;
                uvp[uv_idx]     = yuy2[src_idx + 1];
                uvp[uv_idx + 1] = yuy2[src_idx + 3];
            }
        }
    }
    return true;
}

uint8_t* ConvertI420ToNV12(const uint8_t* i420, int w, int h) {
    size_t y_size = (size_t)w * h;
    size_t nv12_size = y_size + y_size / 2;
    uint8_t* nv12 = (uint8_t*)malloc(nv12_size);
    if (!nv12) return nullptr;

    memcpy(nv12, i420, y_size);
    uint8_t* uvp = nv12 + y_size;
    const uint8_t* up = i420 + y_size;
    const uint8_t* vp = up + (size_t)(w / 2) * (h / 2);
    size_t wu = (size_t)w;
    size_t w2 = (size_t)(w / 2);
    size_t h2 = (size_t)(h / 2);

    for (size_t row = 0; row < h2; row++) {
        for (size_t col = 0; col < w2; col++) {
            uvp[row * wu + col * 2]     = up[row * w2 + col];
            uvp[row * wu + col * 2 + 1] = vp[row * w2 + col];
        }
    }
    return nv12;
}

bool ConvertI420ToNV12(const uint8_t* i420, int w, int h, uint8_t* out, int out_size) {
    size_t y_size = (size_t)w * h;
    size_t nv12_size = y_size + y_size / 2;
    if ((size_t)out_size < nv12_size) return false;

    memcpy(out, i420, y_size);
    uint8_t* uvp = out + y_size;
    const uint8_t* up = i420 + y_size;
    const uint8_t* vp = up + (size_t)(w / 2) * (h / 2);
    size_t wu = (size_t)w;
    size_t w2 = (size_t)(w / 2);
    size_t h2 = (size_t)(h / 2);

    for (size_t row = 0; row < h2; row++) {
        for (size_t col = 0; col < w2; col++) {
            uvp[row * wu + col * 2]     = up[row * w2 + col];
            uvp[row * wu + col * 2 + 1] = vp[row * w2 + col];
        }
    }
    return true;
}

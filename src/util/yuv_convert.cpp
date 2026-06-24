#include "yuv_convert.h"
#include <cstdlib>
#include <cstring>
#include <emmintrin.h>  // SSE2

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
    size_t yuv_row = wu * 2;
    for (size_t row = 0; row < hu; row++) {
        const uint8_t* src = yuy2 + row * yuv_row;
        size_t col = 0;
        for (; col + 16 <= wu; col += 16) {
            __m128i lo = _mm_loadu_si128((const __m128i*)(src + col * 2));
            __m128i hi = _mm_loadu_si128((const __m128i*)(src + col * 2 + 16));
            __m128i y_mask = _mm_set1_epi16(0x00FF);
            __m128i y_lo = _mm_and_si128(lo, y_mask);
            __m128i y_hi = _mm_and_si128(hi, y_mask);
            __m128i y16 = _mm_packus_epi16(y_lo, y_hi);
            _mm_storeu_si128((__m128i*)(yp + row * wu + col), y16);
            if ((row & 1) == 0) {
                __m128i uv_lo = _mm_srli_epi16(lo, 8);
                __m128i uv_hi = _mm_srli_epi16(hi, 8);
                __m128i uv = _mm_packus_epi16(uv_lo, uv_hi);
                _mm_storeu_si128((__m128i*)(uvp + (row / 2) * wu + col), uv);
            }
        }
        for (; col < wu; col += 2) {
            size_t src_idx = col * 2;
            yp[row * wu + col]     = yuy2[row * yuv_row + src_idx];
            yp[row * wu + col + 1] = yuy2[row * yuv_row + src_idx + 2];
            if ((row & 1) == 0) {
                size_t uv_idx = (row / 2) * wu + col;
                uvp[uv_idx]     = yuy2[row * yuv_row + src_idx + 1];
                uvp[uv_idx + 1] = yuy2[row * yuv_row + src_idx + 3];
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
    size_t yuv_row = wu * 2;  // bytes per row of YUY2
    for (size_t row = 0; row < hu; row++) {
        const uint8_t* src = yuy2 + row * yuv_row;
        size_t col = 0;
        // SSE2: process 16 pixels (32 bytes YUY2) at a time
        for (; col + 16 <= wu; col += 16) {
            __m128i lo = _mm_loadu_si128((const __m128i*)(src + col * 2));
            __m128i hi = _mm_loadu_si128((const __m128i*)(src + col * 2 + 16));
            // Extract Y: mask low byte of each 16-bit word, then pack
            __m128i y_mask = _mm_set1_epi16(0x00FF);
            __m128i y_lo = _mm_and_si128(lo, y_mask);
            __m128i y_hi = _mm_and_si128(hi, y_mask);
            __m128i y16 = _mm_packus_epi16(y_lo, y_hi);
            _mm_storeu_si128((__m128i*)(yp + row * wu + col), y16);
            // Extract UV on even rows only
            if ((row & 1) == 0) {
                __m128i uv_lo = _mm_srli_epi16(lo, 8);
                __m128i uv_hi = _mm_srli_epi16(hi, 8);
                __m128i uv = _mm_packus_epi16(uv_lo, uv_hi);
                _mm_storeu_si128((__m128i*)(uvp + (row / 2) * wu + col), uv);
            }
        }
        // Remainder (scalar)
        for (; col < wu; col += 2) {
            size_t src_idx = col * 2;
            yp[row * wu + col]     = yuy2[row * yuv_row + src_idx];
            yp[row * wu + col + 1] = yuy2[row * yuv_row + src_idx + 2];
            if ((row & 1) == 0) {
                size_t uv_idx = (row / 2) * wu + col;
                uvp[uv_idx]     = yuy2[row * yuv_row + src_idx + 1];
                uvp[uv_idx + 1] = yuy2[row * yuv_row + src_idx + 3];
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
        size_t col = 0;
        for (; col + 8 <= w2; col += 8) {
            __m128i u = _mm_loadu_si128((const __m128i*)(up + row * w2 + col));
            __m128i v = _mm_loadu_si128((const __m128i*)(vp + row * w2 + col));
            __m128i uv_lo = _mm_unpacklo_epi8(u, v);
            __m128i uv_hi = _mm_unpackhi_epi8(u, v);
            _mm_storeu_si128((__m128i*)(uvp + row * wu + col * 2), uv_lo);
            _mm_storeu_si128((__m128i*)(uvp + row * wu + col * 2 + 16), uv_hi);
        }
        for (; col < w2; col++) {
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
        size_t col = 0;
        // SSE2: interleave 8 UV pairs at a time
        for (; col + 8 <= w2; col += 8) {
            __m128i u = _mm_loadu_si128((const __m128i*)(up + row * w2 + col));
            __m128i v = _mm_loadu_si128((const __m128i*)(vp + row * w2 + col));
            __m128i uv_lo = _mm_unpacklo_epi8(u, v);
            __m128i uv_hi = _mm_unpackhi_epi8(u, v);
            _mm_storeu_si128((__m128i*)(uvp + row * wu + col * 2), uv_lo);
            _mm_storeu_si128((__m128i*)(uvp + row * wu + col * 2 + 16), uv_hi);
        }
        // Remainder (scalar)
        for (; col < w2; col++) {
            uvp[row * wu + col * 2]     = up[row * w2 + col];
            uvp[row * wu + col * 2 + 1] = vp[row * w2 + col];
        }
    }
    return true;
}

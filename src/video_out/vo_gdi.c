#include "video_out.h"

struct VideoOut {
    HWND hwnd;
    int  win_w, win_h;
};

VideoOut* vo_create(HWND hwnd, int w, int h) {
    VideoOut* vo = calloc(1, sizeof(VideoOut));
    vo->hwnd = hwnd;
    vo->win_w = w;
    vo->win_h = h;
    return vo;
}

void vo_render(VideoOut* vo, const uint8_t* rgb32, int src_w, int src_h) {
    HDC hdc = GetDC(vo->hwnd);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = src_w;
    bmi.bmiHeader.biHeight      = -src_h;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(hdc,
        0, 0, vo->win_w, vo->win_h,
        0, 0, src_w, src_h,
        rgb32, &bmi, DIB_RGB_COLORS, SRCCOPY);

    ReleaseDC(vo->hwnd, hdc);
}

void vo_resize(VideoOut* vo, int w, int h) {
    vo->win_w = w;
    vo->win_h = h;
}

void vo_destroy(VideoOut* vo) {
    free(vo);
}

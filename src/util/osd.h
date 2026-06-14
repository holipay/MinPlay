#pragma once
#include <windows.h>

class OSD {
public:
    OSD() = default;
    ~OSD() = default;

    void Draw(HDC hdc, int win_w, int win_h,
              double position, double duration, int fps);
};

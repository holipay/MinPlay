#pragma once
#include <windows.h>

class OSD {
public:
    OSD() = default;
    ~OSD() = default;

    void Draw(HDC hdc, int, int,
              double position, double duration, int fps);
};

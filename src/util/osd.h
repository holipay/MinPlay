#pragma once
#include <windows.h>

class OSD {
public:
    OSD() = default;
    ~OSD() = default;

    void Draw(HDC hdc, double position, double duration, int fps);
};

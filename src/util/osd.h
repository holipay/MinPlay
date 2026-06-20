#pragma once
#include <windows.h>

class OSD {
public:
    OSD() = default;
    ~OSD() = default;

    void Draw(HDC hdc, double position, double duration, int fps,
              float volume = 1.0f, bool muted = false,
              int track_index = -1, int track_count = 0, const wchar_t* title = nullptr);
};

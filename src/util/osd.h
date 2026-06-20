#pragma once
#include <windows.h>
#include <cstdint>

class OSD {
public:
    OSD();
    ~OSD();

    void Draw(HDC hdc, double position, double duration, int fps,
              float volume = 1.0f, bool muted = false,
              int track_index = -1, int track_count = 0, const wchar_t* title = nullptr);

private:
    HBRUSH vol_brush_ = nullptr;
    HBRUSH muted_brush_ = nullptr;
    char cached_title_[256] = {};
    int cached_track_idx_ = -1;
    int cached_track_count_ = 0;
    double cached_position_ = -1;
    double cached_duration_ = -1;
    int cached_fps_ = -1;
    float cached_volume_ = -1;
    bool cached_muted_ = false;
};

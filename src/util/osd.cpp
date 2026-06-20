#include "osd.h"
#include <cstdio>
#include <cstring>

OSD::OSD() {
    vol_brush_ = CreateSolidBrush(RGB(0, 200, 0));
    muted_brush_ = CreateSolidBrush(RGB(128, 128, 128));
}

OSD::~OSD() {
    if (vol_brush_) DeleteObject(vol_brush_);
    if (muted_brush_) DeleteObject(muted_brush_);
}

void OSD::Draw(HDC hdc,
               double position, double duration, int fps,
               float volume, bool muted,
               int track_index, int track_count, const wchar_t* title) {
    if (!hdc) return;

    // Check if any data changed — skip drawing if nothing changed
    bool title_changed = false;
    if (title && track_count > 0) {
        char title_mb[200];
        WideCharToMultiByte(CP_UTF8, 0, title, -1, title_mb, sizeof(title_mb), nullptr, nullptr);
        title_changed = (strcmp(cached_title_, title_mb) != 0);
        if (title_changed) {
            strncpy_s(cached_title_, title_mb, sizeof(cached_title_) - 1);
            cached_title_[sizeof(cached_title_) - 1] = '\0';
        }
    }
    bool data_changed = title_changed ||
        track_index != cached_track_idx_ || track_count != cached_track_count_ ||
        (int)position != (int)cached_position_ || (int)duration != (int)cached_duration_ ||
        fps != cached_fps_ || volume != cached_volume_ || muted != cached_muted_;

    if (!data_changed) return;  // Nothing changed — skip GDI operations

    // Update cache
    cached_track_idx_ = track_index;
    cached_track_count_ = track_count;
    cached_position_ = position;
    cached_duration_ = duration;
    cached_fps_ = fps;
    cached_volume_ = volume;
    cached_muted_ = muted;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    int y = 10;

    // Show playlist track info
    if (track_count > 0) {
        TextOutA(hdc, 10, y, cached_title_, (int)strlen(cached_title_));
        y += 20;
    }

    // Time / FPS / volume
    char buf[128];
    int pos_min = (int)position / 60;
    int pos_sec = (int)position % 60;
    if (duration < 0) duration = 0;
    int dur_min = (int)duration / 60;
    int dur_sec = (int)duration % 60;

    if (fps < 0) fps = 0;

    int vol_pct = (int)(volume * 100);

    if (muted || volume < 1.0f) {
        snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d  [%d fps]  Vol:%d%%%s",
                 pos_min, pos_sec, dur_min, dur_sec, fps, vol_pct, muted ? " MUTE" : "");
    } else {
        snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d  [%d fps]",
                 pos_min, pos_sec, dur_min, dur_sec, fps);
    }

    TextOutA(hdc, 10, y, buf, (int)strlen(buf));
    y += 20;

    // Draw volume bar when volume is not at 100%
    if (muted || volume < 1.0f) {
        int bar_x = 10;
        int bar_y = y;
        int bar_w = 100;
        int bar_h = 8;

        // Background
        RECT rc_bg = { bar_x, bar_y, bar_x + bar_w, bar_y + bar_h };
        FillRect(hdc, &rc_bg, (HBRUSH)GetStockObject(DKGRAY_BRUSH));

        // Fill (use cached brush — no per-frame CreateSolidBrush/DeleteObject)
        int fill_w = (int)(bar_w * volume);
        if (fill_w > 0) {
            RECT rc_fill = { bar_x, bar_y, bar_x + fill_w, bar_y + bar_h };
            FillRect(hdc, &rc_fill, muted ? muted_brush_ : vol_brush_);
        }
    }
}

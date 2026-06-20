#include "osd.h"
#include <cstdio>
#include <cstring>

void OSD::Draw(HDC hdc,
               double position, double duration, int fps,
               float volume, bool muted,
               int track_index, int track_count, const wchar_t* title) {
    if (!hdc) return;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));

    int y = 10;

    // Show playlist track info
    if (track_count > 0) {
        char track_buf[256];
        if (title) {
            char title_mb[200];
            WideCharToMultiByte(CP_UTF8, 0, title, -1, title_mb, sizeof(title_mb), nullptr, nullptr);
            snprintf(track_buf, sizeof(track_buf), "Track %d/%d: %s",
                     track_index + 1, track_count, title_mb);
        } else {
            snprintf(track_buf, sizeof(track_buf), "Track %d/%d",
                     track_index + 1, track_count);
        }
        TextOutA(hdc, 10, y, track_buf, (int)strlen(track_buf));
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

    const char* vol_str = muted ? "MUTE" : "";
    int vol_pct = (int)(volume * 100);

    if (muted || volume < 1.0f) {
        snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d  [%d fps]  Vol:%d%%%s",
                 pos_min, pos_sec, dur_min, dur_sec, fps, vol_pct, vol_str);
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

        // Fill
        int fill_w = (int)(bar_w * volume);
        if (fill_w > 0) {
            RECT rc_fill = { bar_x, bar_y, bar_x + fill_w, bar_y + bar_h };
            HBRUSH br = CreateSolidBrush(muted ? RGB(128, 128, 128) : RGB(0, 200, 0));
            FillRect(hdc, &rc_fill, br);
            DeleteObject(br);
        }
    }
}

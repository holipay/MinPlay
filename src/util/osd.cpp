#include "osd.h"
#include <cstdio>
#include <cstring>

void OSD::Draw(HDC hdc, int, int,
               double position, double duration, int fps) {
    if (!hdc) return;

    char buf[128];
    int pos_min = (int)position / 60;
    int pos_sec = (int)position % 60;
    if (duration < 0) duration = 0;
    int dur_min = (int)duration / 60;
    int dur_sec = (int)duration % 60;

    snprintf(buf, sizeof(buf), "%02d:%02d / %02d:%02d  [%d fps]",
             pos_min, pos_sec, dur_min, dur_sec, fps);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    TextOutA(hdc, 10, 10, buf, (int)strlen(buf));
}

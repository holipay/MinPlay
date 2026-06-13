#ifndef OSD_H
#define OSD_H

#include <windows.h>

typedef struct OSD OSD;

OSD*    osd_create(void);
void    osd_draw(OSD* osd, HDC hdc, int win_w, int win_h,
                 double position, double duration, int fps);
void    osd_destroy(OSD* osd);

#endif

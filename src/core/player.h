#ifndef PLAYER_H
#define PLAYER_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

typedef struct Player Player;

typedef enum {
    STATE_STOPPED = 0,
    STATE_PLAYING = 1,
    STATE_PAUSED  = 2
} PlayerState;

Player*         player_create(HWND hwnd);
int             player_open(Player* p, const wchar_t* url);
void            player_close(Player* p);
void            player_destroy(Player* p);

void            player_play(Player* p);
void            player_pause_toggle(Player* p);
void            player_stop(Player* p);
void            player_seek(Player* p, double seconds);

PlayerState     player_get_state(Player* p);
double          player_get_duration(Player* p);
double          player_get_position(Player* p);
int             player_has_video(Player* p);
int             player_has_audio(Player* p);

void            player_paint(Player* p, HDC hdc, int w, int h);

void            player_process_video_frame(Player* p, IMFSample* sample, LONGLONG timestamp);
void            player_process_audio_frame(Player* p, IMFSample* sample, LONGLONG timestamp);
void            player_check_audio(Player* p);
int             player_is_audio_done(Player* p);
int             player_is_finished(Player* p);

#endif

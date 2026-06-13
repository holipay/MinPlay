#ifndef MEDIA_SOURCE_H
#define MEDIA_SOURCE_H

#include <stdint.h>
#include <windows.h>

typedef struct {
    uint8_t*    data;
    int         size;
    int         owns_data;

    int         width;
    int         height;
    int         stride;

    int         sample_rate;
    int         channels;
    int         bits_per_sample;

    double      timestamp;
    int         type;
} MediaFrame;

typedef struct MediaSource MediaSource;

typedef struct {
    int     width;
    int     height;
    double  fps;
} VideoInfo;

typedef struct {
    int     sample_rate;
    int     channels;
    int     bits_per_sample;
} AudioInfo;

MediaSource*    media_open(const wchar_t* path_or_url);
void            media_close(MediaSource* src);

int             media_read_video(MediaSource* src, MediaFrame* frame);
int             media_read_audio(MediaSource* src, MediaFrame* frame);
void            media_free_frame(MediaFrame* frame);

int             media_seek(MediaSource* src, double seconds);
double          media_get_duration(MediaSource* src);
double          media_get_position(MediaSource* src);

int             media_has_video(MediaSource* src);
int             media_has_audio(MediaSource* src);
int             media_get_video_info(MediaSource* src, VideoInfo* info);
int             media_get_audio_info(MediaSource* src, AudioInfo* info);

#endif

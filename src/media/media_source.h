#ifndef MEDIA_SOURCE_H
#define MEDIA_SOURCE_H

#include <stdint.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

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

typedef enum { PF_UNKNOWN = 0, PF_RGB32, PF_NV12, PF_YUY2, PF_I420 } PixelFormat;

MediaSource*    media_open(const wchar_t* path_or_url);
MediaSource*    media_open_with_callback(const wchar_t* path_or_url,
                                          IMFSourceReaderCallback* callback);
void            media_close(MediaSource* src);

int             media_seek(MediaSource* src, double seconds);
double          media_get_duration(MediaSource* src);
double          media_get_position(MediaSource* src);

int             media_has_video(MediaSource* src);
int             media_has_audio(MediaSource* src);
int             media_get_video_info(MediaSource* src, VideoInfo* info);
int             media_get_audio_info(MediaSource* src, AudioInfo* info);
PixelFormat     media_get_pixel_format(MediaSource* src);

IMFSourceReader* media_get_reader(MediaSource* src);
DWORD           media_get_video_stream(MediaSource* src);
DWORD           media_get_audio_stream(MediaSource* src);

#endif

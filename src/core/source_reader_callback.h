#ifndef SOURCE_READER_CALLBACK_H
#define SOURCE_READER_CALLBACK_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

struct Player;
typedef struct SourceReaderCallback SourceReaderCallback;
typedef struct AudioOut AudioOut;

SourceReaderCallback* src_cb_create(HWND hwnd);
void                  src_cb_destroy(SourceReaderCallback* cb);

HRESULT src_cb_set_on_source_reader(SourceReaderCallback* cb,
                                     IMFSourceReader* reader,
                                     DWORD video_stream,
                                     DWORD audio_stream);
void    src_cb_set_audio_out(SourceReaderCallback* cb, AudioOut* ao);
void    src_cb_set_player(SourceReaderCallback* cb, struct Player* player);
HRESULT src_cb_start_reading(SourceReaderCallback* cb);
HRESULT src_cb_request_video_read(SourceReaderCallback* cb);
HRESULT src_cb_request_audio_read(SourceReaderCallback* cb);
HRESULT src_cb_stop(SourceReaderCallback* cb);
int     src_cb_is_video_eof(SourceReaderCallback* cb);
int     src_cb_is_audio_eof(SourceReaderCallback* cb);
void    src_cb_consume_video(SourceReaderCallback* cb);

#endif

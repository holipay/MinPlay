#pragma once
#include "../util/com_ptr.h"
#include "../video_out/video_output.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cstdint>

class HlsManager;

struct VideoInfo {
    int width = 0;
    int height = 0;
    double fps = 30.0;
};

struct AudioInfo {
    int sample_rate = 0;
    int channels = 0;
    int bits_per_sample = 0;
};

class MediaSource {
public:
    MediaSource() = default;
    ~MediaSource() { Close(); }

    MediaSource(const MediaSource&) = delete;
    MediaSource& operator=(const MediaSource&) = delete;

    bool Open(const wchar_t* url, IMFSourceReaderCallback* callback = nullptr);
    void Close();

    bool Seek(double seconds);
    double Duration() const { return duration_; }
    double Position() const { return last_position_; }
    bool IsLive() const { return is_live_; }

    bool HasVideo() const { return has_video_; }
    bool HasAudio() const { return has_audio_; }
    VideoInfo GetVideoInfo() const { return vi_; }
    AudioInfo GetAudioInfo() const { return ai_; }
    PixelFormat GetPixelFormat() const { return pix_fmt_; }

    IMFSourceReader* GetReader() const { return reader_.get(); }
    DWORD GetVideoStream() const { return video_stream_; }
    DWORD GetAudioStream() const { return audio_stream_; }

    // Re-read video format after media type change
    void ReconfigureVideo();

    // For live HLS: check if new data arrived after pipeline stalled
    bool HasNewHlsData();
    bool HlsByteStreamHasData();

private:
    ComPtr<IMFSourceReader> reader_;
    HlsManager* hls_ = nullptr;  // non-null when HLS stream is active
    bool has_video_ = false;
    bool has_audio_ = false;
    DWORD video_stream_ = (DWORD)-1;
    DWORD audio_stream_ = (DWORD)-1;
    VideoInfo vi_;
    AudioInfo ai_;
    double duration_ = 0;
    double last_position_ = 0;
    PixelFormat pix_fmt_ = PixelFormat::Unknown;
    bool is_live_ = false;

    bool OpenHls(const wchar_t* url);
    static HRESULT GetUint64Pair(IMFAttributes* attr, REFGUID key,
                                 UINT32* a, UINT32* b);
};

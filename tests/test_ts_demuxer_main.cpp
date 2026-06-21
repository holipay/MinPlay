#include "../src/demux/ts_demuxer.h"
#include "../src/network/hls_stream.h"
#include "../src/util/log.h"
#include <cstdio>

int main() {
    LOG_INFO("=== TsDemuxer HLS Test ===");

    const wchar_t* url = L"https://live.corusdigitaldev.com/groupd/live/49a91e7f-1023-430f-8d66-561055f3d0f7/live.isml/master.m3u8";

    // Open HLS stream
    HlsManager hls;
    LOG_INFO("Opening HLS: %ws", url);
    if (!hls.Open(url)) {
        LOG_ERROR("Failed to open HLS stream");
        return 1;
    }
    LOG_INFO("HLS opened: %s, duration: %.1f s",
             hls.IsLive() ? "LIVE" : "VOD", hls.Duration());

    // Wait for first segment to be downloaded
    HlsByteStream* bs = hls.GetByteStream();
    if (!bs) {
        LOG_ERROR("No byte stream");
        return 1;
    }

    LOG_INFO("Waiting for data...");
    if (!bs->WaitForData(10000)) {
        LOG_ERROR("No data after 10s");
        return 1;
    }

    // Create TsDemuxer
    TsDemuxer demuxer;
    LOG_INFO("TsDemuxer created");

    // Read and parse TS data
    int total_frames = 0;
    int video_frames = 0;
    int audio_frames = 0;

    for (int i = 0; i < 100; i++) {  // Read up to 100 chunks
        if (!demuxer.ReadAndDemux(bs)) {
            LOG_DEBUG("ReadAndDemux returned false (no data or error)");
            // Wait a bit and try again
            Sleep(100);
            continue;
        }

        DemuxFrame frame;
        while (demuxer.GetNextFrame(frame)) {
            total_frames++;
            if (frame.type == DemuxFrame::Video) {
                video_frames++;
                if (video_frames <= 5) {
                    LOG_INFO("Video frame #%d: %zu bytes, pts=%.3f, dts=%.3f",
                             video_frames, frame.data.size(), frame.pts, frame.dts);
                }
            } else if (frame.type == DemuxFrame::Audio) {
                audio_frames++;
                if (audio_frames <= 5) {
                    LOG_INFO("Audio frame #%d: %zu bytes, pts=%.3f, dts=%.3f",
                             audio_frames, frame.data.size(), frame.pts, frame.dts);
                }
            }
        }

        if (total_frames > 50) break;  // Stop after enough frames
    }

    LOG_INFO("=== Test Results ===");
    LOG_INFO("Total frames: %d", total_frames);
    LOG_INFO("Video frames: %d", video_frames);
    LOG_INFO("Audio frames: %d", audio_frames);

    if (video_frames > 0 && audio_frames > 0) {
        LOG_INFO("SUCCESS: TsDemuxer parsed both video and audio frames");
    } else if (video_frames > 0 || audio_frames > 0) {
        LOG_INFO("PARTIAL: TsDemuxer parsed some frames");
    } else {
        LOG_ERROR("FAILED: No frames parsed");
    }

    return 0;
}

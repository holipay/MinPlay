#pragma once
#include "ts_packet_parser.h"
#include "pes_assembler.h"
#include "program_manager.h"
#include <vector>
#include <map>

// Forward declarations
class HlsByteStream;

struct DemuxFrame {
    enum Type { Video, Audio };
    Type type;
    std::vector<uint8_t> data;
    double pts;                    // Presentation time in seconds
    double dts;                    // Decode time in seconds
    bool has_pts;
    bool has_dts;
};

class TsDemuxer {
public:
    TsDemuxer();
    ~TsDemuxer();

    // Reset state for new stream
    void Reset();

    // Read and parse TS data from byte stream
    bool ReadAndDemux(HlsByteStream* bs);

    // Feed raw TS bytes directly (for TsByteStream integration)
    bool FeedData(const uint8_t* data, int size);

    // Get next demuxed frame
    bool GetNextFrame(DemuxFrame& frame);

    // Check if frames are available
    bool HasFrames() const;

    // Check if stream is EOS (for diagnostic purposes)
    bool IsEos() const { return eos_; }

    // Get stream info
    bool HasVideo() const { return video_pid_ > 0; }
    bool HasAudio() const { return audio_pid_ > 0; }
    StreamType GetVideoStreamType() const { return video_stream_type_; }
    StreamType GetAudioStreamType() const { return audio_stream_type_; }

    // Codec config extraction for MF's decoder MFTs
    // Returns true and fills out_buf with AVCDecoderConfigurationRecord (avcC) data
    bool GetVideoCodecConfig(std::vector<uint8_t>& out_buf) const;
    // Returns true and fills out_buf with AudioSpecificConfig data
    bool GetAudioCodecConfig(std::vector<uint8_t>& out_buf) const;
    // Video resolution extracted from SPS (valid after first frame is processed)
    int GetVideoWidth() const { return video_width_; }
    int GetVideoHeight() const { return video_height_; }

private:
    TsPacketParser parser_;
    PesAssembler video_assembler_;
    PesAssembler audio_assembler_;
    ProgramManager program_manager_;

    // Stream state
    uint16_t video_pid_;
    uint16_t audio_pid_;
    StreamType video_stream_type_;
    StreamType audio_stream_type_;
    bool program_ready_;

    // Sync state - continuity counter tracking
    std::map<uint16_t, uint8_t> continuity_counters_;

    // Frame queue
    std::vector<DemuxFrame> frames_;
    bool eos_;

    // Read buffer
    std::vector<uint8_t> read_buffer_;

    // Residual bytes from previous read (partial TS packet)
    std::vector<uint8_t> residual_;

    // Extracted codec config (populated from first frames)
    std::vector<uint8_t> video_codec_config_;  // AVCDecoderConfigurationRecord
    std::vector<uint8_t> audio_codec_config_;  // AudioSpecificConfig
    bool video_config_extracted_ = false;
    bool audio_config_extracted_ = false;
    int video_width_ = 0;
    int video_height_ = 0;
};

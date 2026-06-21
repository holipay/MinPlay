#pragma once
#include <cstdint>
#include <vector>
#include <map>

enum class StreamType : uint8_t {
    Unknown = 0,
    H264 = 0x1B,      // H.264 video
    AAC = 0x0F,       // AAC audio (ADTS)
    AAC_LATM = 0x11,  // AAC audio (LATM)
    MP3 = 0x03,       // MP3 audio
};

struct StreamInfo {
    uint16_t pid;
    StreamType type;
};

class ProgramManager {
public:
    ProgramManager();

    // Reset state
    void Reset();

    // Process a TS packet for PAT/PMT
    bool ProcessPacket(const uint8_t* payload, int size, uint16_t pid, bool unit_start);

    // Get video PID
    uint16_t GetVideoPid() const;

    // Get audio PID
    uint16_t GetAudioPid() const;

    // Get all stream info
    const std::vector<StreamInfo>& GetStreams() const { return streams_; }

    // Check if program is ready (PAT + PMT parsed)
    bool IsReady() const;

private:
    bool ParsePat(const uint8_t* data, int size);
    bool ParsePmt(const uint8_t* data, int size, uint16_t pid);

    uint16_t pat_pid_;
    uint16_t pmt_pid_;
    bool pat_parsed_;
    bool pmt_parsed_;

    std::vector<StreamInfo> streams_;
    uint16_t video_pid_;
    uint16_t audio_pid_;
};

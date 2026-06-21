#pragma once
#include <cstdint>
#include <vector>

struct PesFrame {
    std::vector<uint8_t> data;    // Elementary stream data
    double pts;                    // Presentation Time Stamp (seconds)
    double dts;                    // Decode Time Stamp (seconds)
    bool has_pts;                  // PTS present
    bool has_dts;                  // DTS present
    int stream_id;                 // Stream ID (0xE0=video, 0xC0=audio)
};

class PesAssembler {
public:
    PesAssembler();

    // Reset state for new stream
    void Reset();

    // Feed a TS payload into the PES assembler
    // Returns true if a complete PES frame was assembled
    bool FeedPayload(const uint8_t* payload, int size, bool unit_start, uint16_t pid);

    // Get the next assembled PES frame
    bool GetNextFrame(PesFrame& frame);

    // Check if there are frames available
    bool HasFrames() const;

private:
    // Current PES buffer
    std::vector<uint8_t> buffer_;
    bool in_progress_;          // Currently assembling a PES packet
    int expected_pes_len_;      // Expected PES packet length
    int current_pes_len_;       // Current PES packet length

    // Assembled frames queue
    std::vector<PesFrame> frames_;

    // Parse PES header and extract timestamps
    bool ParsePesHeader(const uint8_t* data, int size, PesFrame& frame);

    // Extract PTS from PES header (33-bit value in 5 bytes)
    double ExtractPts(const uint8_t* pts_bytes);
};

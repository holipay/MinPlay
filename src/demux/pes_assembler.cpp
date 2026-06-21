#include "pes_assembler.h"
#include <cstring>

PesAssembler::PesAssembler() {
    Reset();
}

void PesAssembler::Reset() {
    buffer_.clear();
    in_progress_ = false;
    expected_pes_len_ = 0;
    current_pes_len_ = 0;
    frames_.clear();
}

double PesAssembler::ExtractPts(const uint8_t* pts_bytes) {
    // PTS is 33 bits encoded in 5 bytes
    // Format: 4bits '0010' + 3bits PTS[32..30] + 1bit marker
    //         8bits PTS[29..22] + 1bit marker
    //         8bits PTS[21..15] + 1bit marker
    //         8bits PTS[14..7] + 1bit marker
    //         7bits PTS[6..0] + 1bit marker
    uint64_t pts = 0;
    pts = ((uint64_t)(pts_bytes[0] & 0x0E) << 29) |
          ((uint64_t)(pts_bytes[1]) << 22) |
          ((uint64_t)(pts_bytes[2] & 0xFE) << 14) |
          ((uint64_t)(pts_bytes[3]) << 7) |
          ((uint64_t)(pts_bytes[4] >> 1));

    return (double)pts / 90000.0;  // Convert from 90kHz clock to seconds
}

bool PesAssembler::ParsePesHeader(const uint8_t* data, int size, PesFrame& frame) {
    if (size < 9) return false;

    // PES start code: 00 00 01
    if (data[0] != 0x00 || data[1] != 0x00 || data[2] != 0x01) return false;

    frame.stream_id = data[3];
    frame.has_pts = false;
    frame.has_dts = false;
    frame.pts = 0;
    frame.dts = 0;

    // Only process video (0xE0-0xEF) and audio (0xC0-0xDF) streams
    if (frame.stream_id < 0xC0 || frame.stream_id > 0xEF) return false;

    // Parse PES header
    int header_len = 9 + data[8];  // 9-byte fixed header + optional header length
    if (header_len > size) return false;

    // Check for PTS/DTS flags
    uint8_t pts_dts_flags = (data[7] >> 6) & 0x03;

    if (pts_dts_flags == 0x02 || pts_dts_flags == 0x03) {
        // PTS present
        if (header_len >= 14) {
            frame.pts = ExtractPts(data + 9);
            frame.has_pts = true;
        }
    }

    if (pts_dts_flags == 0x03) {
        // DTS present (after PTS)
        if (header_len >= 19) {
            frame.dts = ExtractPts(data + 14);
            frame.has_dts = true;
        }
    }

    // Extract ES data
    frame.data.assign(data + header_len, data + size);

    return true;
}

bool PesAssembler::FeedPayload(const uint8_t* payload, int size, bool unit_start, uint16_t pid) {
    if (size <= 0) return false;

    if (unit_start) {
        // New PES packet starts
        if (in_progress_ && !buffer_.empty()) {
            // Complete the previous PES packet
            PesFrame frame;
            if (ParsePesHeader(buffer_.data(), (int)buffer_.size(), frame)) {
                frames_.push_back(std::move(frame));
            }
            buffer_.clear();
        }
        in_progress_ = true;
    }

    if (!in_progress_) return false;

    // Append payload to buffer
    buffer_.insert(buffer_.end(), payload, payload + size);

    // Try to parse the PES header to get the expected length
    if (buffer_.size() >= 6) {
        expected_pes_len_ = (buffer_[4] << 8) | buffer_[5];
        if (expected_pes_len_ > 0) {
            expected_pes_len_ += 6;  // Include the 6-byte PES header
        }
    }

    // Check if we have a complete PES packet
    if (expected_pes_len_ > 0 && (int)buffer_.size() >= expected_pes_len_) {
        PesFrame frame;
        if (ParsePesHeader(buffer_.data(), expected_pes_len_, frame)) {
            frames_.push_back(std::move(frame));
        }
        // Remove consumed data, keep any excess
        int remaining = (int)buffer_.size() - expected_pes_len_;
        if (remaining > 0) {
            std::memmove(buffer_.data(), buffer_.data() + expected_pes_len_, remaining);
            buffer_.resize(remaining);
        } else {
            buffer_.clear();
        }
        in_progress_ = false;
        expected_pes_len_ = 0;
        return true;
    }

    return false;
}

bool PesAssembler::GetNextFrame(PesFrame& frame) {
    if (frames_.empty()) return false;
    frame = std::move(frames_.front());
    frames_.erase(frames_.begin());
    return true;
}

bool PesAssembler::HasFrames() const {
    return !frames_.empty();
}

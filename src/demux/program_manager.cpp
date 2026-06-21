#include "program_manager.h"
#include <cstring>

ProgramManager::ProgramManager() {
    Reset();
}

void ProgramManager::Reset() {
    pat_pid_ = 0;      // PAT is always PID 0
    pmt_pid_ = 0;
    pat_parsed_ = false;
    pmt_parsed_ = false;
    streams_.clear();
    video_pid_ = 0;
    audio_pid_ = 0;
}

bool ProgramManager::ProcessPacket(const uint8_t* payload, int size, uint16_t pid, bool unit_start) {
    if (!unit_start) return false;  // Only process first payload of each section

    if (pid == 0 && !pat_parsed_) {
        return ParsePat(payload, size);
    } else if (pid == pmt_pid_ && pmt_pid_ > 0 && !pmt_parsed_) {
        return ParsePmt(payload, size, pid);
    }
    return false;
}

bool ProgramManager::ParsePat(const uint8_t* data, int size) {
    if (size < 8) return false;

    // Skip pointer field (1 byte)
    int offset = 1;

    // Table ID (should be 0x00 for PAT)
    if (data[offset] != 0x00) return false;
    offset++;

    // Section length (12 bits)
    uint16_t section_length = ((data[offset] & 0x0F) << 8) | data[offset + 1];
    offset += 2;

    // Transport stream ID (16 bits) - skip
    offset += 2;

    // Version number (5 bits) + current next indicator (1 bit) - skip
    offset += 1;

    // Section number (8 bits) - skip
    offset += 1;

    // Last section number (8 bits) - skip
    offset += 1;

    // Parse program list
    int end = offset + section_length - 4;  // -4 for CRC
    while (offset + 4 <= end && offset + 4 <= size) {
        uint16_t program_number = (data[offset] << 8) | data[offset + 1];
        uint16_t pid = ((data[offset + 2] & 0x1F) << 8) | data[offset + 3];

        if (program_number != 0 && pid > 0) {
            pmt_pid_ = pid;
            pat_parsed_ = true;
            return true;
        }
        offset += 4;
    }

    return false;
}

bool ProgramManager::ParsePmt(const uint8_t* data, int size, uint16_t pid) {
    if (size < 12) return false;

    // Skip pointer field
    int offset = 1;

    // Table ID (should be 0x02 for PMT)
    if (data[offset] != 0x02) return false;
    offset++;

    // Section length
    uint16_t section_length = ((data[offset] & 0x0F) << 8) | data[offset + 1];
    offset += 2;

    // Program number (16 bits) - skip
    offset += 2;

    // Version + current next - skip
    offset += 1;

    // Section number + last section number - skip
    offset += 2;

    // PCR PID (13 bits)
    offset += 2;

    // Program info length (12 bits)
    uint16_t prog_info_len = ((data[offset] & 0x0F) << 8) | data[offset + 1];
    offset += 2;
    offset += prog_info_len;

    // Parse stream list
    int end = offset + section_length - 4 - 9 - prog_info_len;
    streams_.clear();

    while (offset + 5 <= end && offset + 5 <= size) {
        uint8_t stream_type = data[offset];
        uint16_t elementary_pid = ((data[offset + 1] & 0x1F) << 8) | data[offset + 2];

        StreamType type = StreamType::Unknown;
        if (stream_type == 0x1B) type = StreamType::H264;
        else if (stream_type == 0x0F) type = StreamType::AAC;
        else if (stream_type == 0x11) type = StreamType::AAC_LATM;
        else if (stream_type == 0x03) type = StreamType::MP3;

        if (type != StreamType::Unknown) {
            streams_.push_back({elementary_pid, type});
        }

        // ES info length (12 bits)
        uint16_t es_info_len = ((data[offset + 3] & 0x0F) << 8) | data[offset + 4];
        offset += 5 + es_info_len;
    }

    // Find video and audio PIDs
    video_pid_ = 0;
    audio_pid_ = 0;

    for (const auto& s : streams_) {
        if (s.type == StreamType::H264 && video_pid_ == 0) {
            video_pid_ = s.pid;
        } else if ((s.type == StreamType::AAC || s.type == StreamType::AAC_LATM ||
                    s.type == StreamType::MP3) && audio_pid_ == 0) {
            audio_pid_ = s.pid;
        }
    }

    if (video_pid_ > 0 || audio_pid_ > 0) {
        pmt_parsed_ = true;
        return true;
    }

    return false;
}

uint16_t ProgramManager::GetVideoPid() const { return video_pid_; }
uint16_t ProgramManager::GetAudioPid() const { return audio_pid_; }
bool ProgramManager::IsReady() const { return pat_parsed_ && pmt_parsed_; }

#include "ts_demuxer.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <algorithm>

TsDemuxer::TsDemuxer()
    : video_pid_(0), audio_pid_(0), program_ready_(false), eos_(false) {
    read_buffer_.reserve(256 * 1024);  // 256KB read buffer
}

TsDemuxer::~TsDemuxer() {}

void TsDemuxer::Reset() {
    parser_.Reset();
    video_assembler_.Reset();
    audio_assembler_.Reset();
    program_manager_.Reset();
    video_pid_ = 0;
    audio_pid_ = 0;
    program_ready_ = false;
    continuity_counters_.clear();
    frames_.clear();
    eos_ = false;
    read_buffer_.clear();
}

bool TsDemuxer::ReadAndDemux(HlsByteStream* bs) {
    if (!bs || eos_) return false;

    // Read a chunk of data from the byte stream
    const int READ_SIZE = 256 * 1024;  // 256KB chunks
    read_buffer_.resize(READ_SIZE);

    ULONG bytes_read = 0;
    HRESULT hr = bs->Read(read_buffer_.data(), (ULONG)read_buffer_.size(), &bytes_read);

    if (FAILED(hr) || bytes_read == 0) {
        // No data available - this is normal for live streams
        // The caller should wait and try again
        return false;
    }

    const uint8_t* data = read_buffer_.data();
    int data_size = (int)bytes_read;

    // Find first sync byte
    int pos = parser_.FindSyncByte(data, data_size, 0);
    if (pos < 0) {
        LOG_WARN("TsDemuxer: no sync byte found in %d bytes", data_size);
        return false;
    }

    // Process complete TS packets
    while (pos + TS_PACKET_SIZE <= data_size) {
        TsPacket packet;
        if (parser_.ParsePacket(data + pos, data_size - pos, packet)) {
            // Process PAT/PMT
            if (!program_ready_) {
                if (program_manager_.ProcessPacket(packet.payload, packet.payload_size,
                                                    packet.pid, packet.payload_unit_start)) {
                    if (program_manager_.IsReady()) {
                        video_pid_ = program_manager_.GetVideoPid();
                        audio_pid_ = program_manager_.GetAudioPid();
                        program_ready_ = true;
                        LOG_INFO("TsDemuxer: program ready — video PID=%u, audio PID=%u",
                                 video_pid_, audio_pid_);
                    }
                }
            }

            // Check continuity counter (optional - for debugging)
            auto it = continuity_counters_.find(packet.pid);
            if (it != continuity_counters_.end()) {
                uint8_t expected = (it->second + 1) & 0x0F;
                if (expected != packet.continuity_counter) {
                    LOG_DEBUG("TsDemuxer: PID %u continuity error (expected %u, got %u)",
                              packet.pid, expected, packet.continuity_counter);
                }
            }
            continuity_counters_[packet.pid] = packet.continuity_counter;

            // Feed to PES assembler
            if (packet.pid == video_pid_ && video_pid_ > 0) {
                video_assembler_.FeedPayload(packet.payload, packet.payload_size,
                                             packet.payload_unit_start, packet.pid);
            } else if (packet.pid == audio_pid_ && audio_pid_ > 0) {
                audio_assembler_.FeedPayload(packet.payload, packet.payload_size,
                                             packet.payload_unit_start, packet.pid);
            }
        }

        pos += TS_PACKET_SIZE;

        // Re-align to next sync byte
        if (pos + TS_PACKET_SIZE <= data_size) {
            int next_sync = parser_.FindSyncByte(data, data_size, pos);
            if (next_sync > pos) {
                pos = next_sync;
            } else if (next_sync < 0) {
                break;  // No more sync bytes
            }
        }
    }

    return true;
}

bool TsDemuxer::GetNextFrame(DemuxFrame& frame) {
    // Try video first
    PesFrame pes_frame;
    if (video_assembler_.GetNextFrame(pes_frame)) {
        frame.type = DemuxFrame::Video;
        frame.data = std::move(pes_frame.data);
        frame.pts = pes_frame.pts;
        frame.dts = pes_frame.dts;
        frame.has_pts = pes_frame.has_pts;
        frame.has_dts = pes_frame.has_dts;
        return true;
    }

    // Then audio
    if (audio_assembler_.GetNextFrame(pes_frame)) {
        frame.type = DemuxFrame::Audio;
        frame.data = std::move(pes_frame.data);
        frame.pts = pes_frame.pts;
        frame.dts = pes_frame.dts;
        frame.has_pts = pes_frame.has_pts;
        frame.has_dts = pes_frame.has_dts;
        return true;
    }

    return false;
}

bool TsDemuxer::HasFrames() const {
    return video_assembler_.HasFrames() || audio_assembler_.HasFrames();
}

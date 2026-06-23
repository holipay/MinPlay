#include "ts_demuxer.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <algorithm>

TsDemuxer::TsDemuxer()
    : video_pid_(0), audio_pid_(0), video_stream_type_(StreamType::Unknown),
      audio_stream_type_(StreamType::Unknown),
      program_ready_(false), eos_(false) {
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

    const int READ_SIZE = 256 * 1024;
    read_buffer_.resize(READ_SIZE);

    ULONG bytes_read = 0;
    HRESULT hr = bs->Read(read_buffer_.data(), (ULONG)read_buffer_.size(), &bytes_read);

    if (FAILED(hr) || bytes_read == 0) {
        return false;
    }

    // Prepend residual bytes from previous read
    if (!residual_.empty()) {
        int total = (int)residual_.size() + (int)bytes_read;
        std::vector<uint8_t> combined(total);
        memcpy(combined.data(), residual_.data(), residual_.size());
        memcpy(combined.data() + residual_.size(), read_buffer_.data(), bytes_read);
        residual_.clear();
        return FeedData(combined.data(), total);
    }

    return FeedData(read_buffer_.data(), (int)bytes_read);
}

bool TsDemuxer::FeedData(const uint8_t* data, int size) {
    if (!data || size <= 0 || eos_) return false;

    const uint8_t* raw = data;
    int raw_size = size;

    // Find first sync byte
    int pos = parser_.FindSyncByte(raw, raw_size, 0);
    if (pos < 0) {
        LOG_WARN("TsDemuxer: no sync byte found in %d bytes", raw_size);
        return false;
    }

    // Process complete TS packets
    while (pos + TS_PACKET_SIZE <= raw_size) {
        TsPacket packet;
        if (parser_.ParsePacket(raw + pos, raw_size - pos, packet)) {
            // Process PAT/PMT
            if (!program_ready_) {
                if (program_manager_.ProcessPacket(packet.payload, packet.payload_size,
                                                    packet.pid, packet.payload_unit_start)) {
                    if (program_manager_.IsReady()) {
                        video_pid_ = program_manager_.GetVideoPid();
                        audio_pid_ = program_manager_.GetAudioPid();
                        program_ready_ = true;
                        // Resolve stream types from ProgramManager
                        for (const auto& s : program_manager_.GetStreams()) {
                            if (s.pid == video_pid_ && video_pid_ > 0)
                                video_stream_type_ = s.type;
                            else if (s.pid == audio_pid_ && audio_pid_ > 0)
                                audio_stream_type_ = s.type;
                        }
                        LOG_INFO("TsDemuxer: program ready — video PID=%u type=%d, audio PID=%u type=%d",
                                 video_pid_, (int)video_stream_type_, audio_pid_, (int)audio_stream_type_);
                    }
                }
            }

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
        if (pos + TS_PACKET_SIZE <= raw_size) {
            int next_sync = parser_.FindSyncByte(raw, raw_size, pos);
            if (next_sync > pos) {
                pos = next_sync;
            } else if (next_sync < 0) {
                break;
            }
        }
    }

    // Save residual bytes (partial TS packet at end of buffer)
    if (pos < raw_size) {
        residual_.assign(raw + pos, raw + raw_size);
    } else {
        residual_.clear();
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

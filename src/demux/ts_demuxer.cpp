#include "ts_demuxer.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <algorithm>

// Exp-Golomb bit reader for H.264 SPS parsing
struct BitReader {
    const uint8_t* data;
    int size;
    int bit_pos;
    BitReader(const uint8_t* d, int sz) : data(d), size(sz), bit_pos(0) {}
    int readBit() {
        if (bit_pos / 8 >= size) return 0;
        int val = (data[bit_pos / 8] >> (7 - (bit_pos % 8))) & 1;
        bit_pos++;
        return val;
    }
    // read Unsigned Exp-Golomb
    int readUE() {
        int zeros = 0;
        while (readBit() == 0 && zeros < 32) zeros++;
        if (zeros == 0) return 0;
        int val = 0;
        for (int i = zeros - 1; i >= 0; i--) val |= readBit() << i;
        return val + ((1 << zeros) - 1);
    }
    // read Signed Exp-Golomb
    int readSE() {
        int val = readUE();
        return (val & 1) ? (val + 1) / 2 : -(val / 2);
    }
    void skipBits(int n) { bit_pos += n; }
};

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
                        // Set codec types for output formatting
                        if (video_pid_ > 0) video_assembler_.SetCodec(StreamCodec::H264);
                        if (audio_pid_ > 0) audio_assembler_.SetCodec(StreamCodec::AAC);
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

        // Extract H.264 SPS/PPS from first video frame for MF decoder config
        if (!video_config_extracted_ && frame.data.size() > 4) {
            video_config_extracted_ = true;
            // Parse Annex B NAL units to find SPS (type 7) and PPS (type 8)
            std::vector<uint8_t> sps, pps;
            const uint8_t* d = frame.data.data();
            int sz = (int)frame.data.size();
            int i = 0;
            while (i + 4 < sz) {
                // Find start code 00 00 00 01 or 00 00 01
                int sc_len = 0;
                if (d[i] == 0 && d[i+1] == 0) {
                    if (d[i+2] == 0 && d[i+3] == 1) sc_len = 4;
                    else if (d[i+2] == 1) sc_len = 3;
                }
                if (sc_len == 0) { i++; continue; }

                int nal_start = i + sc_len;
                if (nal_start >= sz) break;
                uint8_t nal_type = d[nal_start] & 0x1F;

                // Find next start code to determine NAL end
                int nal_end = sz;
                for (int j = nal_start + 1; j + 3 < sz; j++) {
                    if (d[j] == 0 && d[j+1] == 0 && (d[j+2] == 1 || (d[j+2] == 0 && d[j+3] == 1))) {
                        nal_end = j;
                        break;
                    }
                }

                if (nal_type == 7) { // SPS
                    sps.assign(d + nal_start, d + nal_end);
                } else if (nal_type == 8) { // PPS
                    pps.assign(d + nal_start, d + nal_end);
                }

                i = nal_end;
            }

            if (!sps.empty() && !pps.empty()) {
                // Build AVCDecoderConfigurationRecord (avcC format)
                uint8_t profile = sps.size() > 1 ? sps[1] : 66;
                uint8_t compat = sps.size() > 2 ? sps[2] : 0xC0;
                uint8_t level = sps.size() > 3 ? sps[3] : 30;
                video_codec_config_.clear();
                video_codec_config_.push_back(1);           // configurationVersion
                video_codec_config_.push_back(profile);      // AVCProfileIndication
                video_codec_config_.push_back(compat);       // profile_compatibility
                video_codec_config_.push_back(level);        // AVCLevelIndication
                video_codec_config_.push_back(0xFF);         // lengthSizeMinusOne = 3 (4-byte NAL length)
                video_codec_config_.push_back(0xE1);         // numOfSPS = 1, reserved bits
                video_codec_config_.push_back((uint8_t)(sps.size() >> 8));
                video_codec_config_.push_back((uint8_t)(sps.size() & 0xFF));
                video_codec_config_.insert(video_codec_config_.end(), sps.begin(), sps.end());
                video_codec_config_.push_back(1);            // numOfPPS = 1
                video_codec_config_.push_back((uint8_t)(pps.size() >> 8));
                video_codec_config_.push_back((uint8_t)(pps.size() & 0xFF));
                video_codec_config_.insert(video_codec_config_.end(), pps.begin(), pps.end());
                LOG_INFO("TsDemuxer: extracted H.264 SPS (%zu bytes) + PPS (%zu bytes)", sps.size(), pps.size());

                // Parse SPS to extract resolution for MF's decoder MFT
                // SPS starts after 1-byte NAL header (forbidden_zero_bit + nal_ref_idc + nal_type)
                if (sps.size() > 4) {
                    BitReader br(sps.data() + 1, (int)sps.size() - 1);
                    // Skip profile_idc (8), constraint_set_flags (8), level_idc (8)
                    br.skipBits(24);
                    br.readUE();  // seq_parameter_set_id
                    // For high profile (100, 110, 122, 244, 44, 83, 86, 118, 128):
                    uint8_t profile_idc = sps[1];
                    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
                        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
                        profile_idc == 86 || profile_idc == 118 || profile_idc == 128) {
                        int chroma_format_idc = br.readUE();
                        if (chroma_format_idc == 3) br.readBit();  // separate_colour_plane_flag
                        br.readUE();  // bit_depth_luma_minus8
                        br.readUE();  // bit_depth_chroma_minus8
                        br.readBit();  // qpprime_y_zero_transform_bypass_flag
                        int seq_scaling_matrix_present = br.readBit();
                        if (seq_scaling_matrix_present) {
                            int cnt = (chroma_format_idc != 3) ? 8 : 12;
                            for (int k = 0; k < cnt; k++) {
                                if (br.readBit()) {
                                    int sl = (k < 6) ? 16 : 64;
                                    int last = 8, next = 8;
                                    for (int j = 0; j < sl; j++) {
                                        if (next != 0) next = (last + br.readSE() + 256) % 256;
                                        last = (next == 0) ? last : next;
                                    }
                                }
                            }
                        }
                    }
                    br.readUE();  // log2_max_frame_num_minus4
                    int pic_order_cnt_type = br.readUE();
                    if (pic_order_cnt_type == 0) {
                        br.readUE();  // log2_max_pic_order_cnt_lsb_minus4
                    } else if (pic_order_cnt_type == 1) {
                        br.readBit();  // delta_pic_order_always_zero_flag
                        br.readSE();   // offset_for_non_ref_pic
                        br.readSE();   // offset_for_top_to_bottom_field
                        int num_ref = br.readUE();
                        for (int k = 0; k < num_ref; k++) br.readSE();  // offset_for_ref_frame
                    }
                    br.readUE();  // max_num_ref_frames
                    br.readBit();  // gaps_in_frame_num_value_allowed_flag
                    int pic_width_mbs = br.readUE() + 1;
                    int pic_height_map = br.readUE() + 1;
                    int frame_mbs_only = br.readBit();
                    if (!frame_mbs_only) br.readBit();  // mb_adaptive_frame_field_flag
                    br.readBit();  // direct_8x8_inference_flag

                    // Cropping
                    int crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
                    if (br.readBit()) {  // frame_cropping_flag
                        crop_left = br.readUE();
                        crop_right = br.readUE();
                        crop_top = br.readUE();
                        crop_bottom = br.readUE();
                    }

                    int width = pic_width_mbs * 16;
                    int height = (2 - frame_mbs_only) * pic_height_map * 16;
                    // Apply cropping (crop values are in 2-pixel units for 4:2:0)
                    int crop_h = crop_left + crop_right;
                    int crop_v = crop_top + crop_bottom;
                    if (profile_idc == 44 || profile_idc == 83 || profile_idc == 86 ||
                        profile_idc == 100 || profile_idc == 110 || profile_idc == 118 ||
                        profile_idc == 128 || profile_idc == 122 || profile_idc == 244) {
                        // Monochrome or high chroma — crop in 2-pixel units for luma
                        // but for 4:2:0 chroma_format_idc==1, crop is in 2-pixel units
                        crop_h *= 2;
                        crop_v *= 2;
                    }
                    width -= crop_h;
                    height -= crop_v;
                    if (width > 0 && height > 0) {
                        video_width_ = width;
                        video_height_ = height;
                        LOG_INFO("TsDemuxer: H.264 resolution from SPS: %dx%d", width, height);
                    }
                }
            }
        }
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

        // Extract AAC AudioSpecificConfig from first audio frame
        if (!audio_config_extracted_ && frame.data.size() > 2) {
            audio_config_extracted_ = true;
            // ADTS header: 7 or 9 bytes. AudioSpecificConfig is in the header.
            // Bytes 2-3 of ADTS contain: profile(2 bits), freq_index(4 bits), channels(4 bits)
            // But in raw AAC (LATM in TS), the first 2 bytes ARE the AudioSpecificConfig
            if (frame.data[0] == 0xFF && (frame.data[1] & 0xF0) == 0xF0) {
                // ADTS header present — extract AudioSpecificConfig from ADTS
                uint8_t profile = ((frame.data[2] >> 6) & 0x03) + 1;
                uint8_t freq_idx = (frame.data[2] >> 2) & 0x0F;
                uint8_t channels = (frame.data[2] & 0x01) << 2 | ((frame.data[3] >> 6) & 0x03);
                uint16_t asc = (uint16_t)((profile << 11) | (freq_idx << 7) | (channels << 3));
                audio_codec_config_.push_back((uint8_t)(asc >> 8));
                audio_codec_config_.push_back((uint8_t)(asc & 0xFF));
                LOG_INFO("TsDemuxer: extracted AAC AudioSpecificConfig from ADTS (profile=%u, freq=%u, ch=%u)",
                         profile, freq_idx, channels);
            } else {
                // Raw AAC — first 2 bytes are AudioSpecificConfig
                audio_codec_config_.assign(frame.data.begin(), frame.data.begin() + 2);
                LOG_INFO("TsDemuxer: extracted AAC AudioSpecificConfig (raw): %02X %02X",
                         frame.data[0], frame.data[1]);
            }
        }
        return true;
    }

    return false;
}

bool TsDemuxer::HasFrames() const {
    return video_assembler_.HasFrames() || audio_assembler_.HasFrames();
}

bool TsDemuxer::GetVideoCodecConfig(std::vector<uint8_t>& out_buf) const {
    if (video_codec_config_.empty()) return false;
    out_buf = video_codec_config_;
    return true;
}

bool TsDemuxer::GetAudioCodecConfig(std::vector<uint8_t>& out_buf) const {
    if (audio_codec_config_.empty()) return false;
    out_buf = audio_codec_config_;
    return true;
}

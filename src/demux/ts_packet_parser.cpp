#include "ts_packet_parser.h"
#include <cstring>

int TsPacketParser::FindSyncByte(const uint8_t* data, int size, int start_offset) {
    for (int i = start_offset; i < size; i++) {
        if (data[i] == TS_SYNC_BYTE) {
            // Verify it's a valid sync by checking next sync at i+188
            if (i + TS_PACKET_SIZE <= size && data[i + TS_PACKET_SIZE] == TS_SYNC_BYTE) {
                return i;
            }
        }
    }
    return -1;
}

bool TsPacketParser::ParsePacket(const uint8_t* data, int size, TsPacket& packet) {
    if (size < TS_PACKET_SIZE) return false;

    // Verify sync byte
    if (data[0] != TS_SYNC_BYTE) return false;

    // Parse TS header (4 bytes)
    uint8_t b0 = data[1];
    uint8_t b1 = data[2];
    uint8_t b2 = data[3];

    packet.pid = ((b0 & 0x1F) << 8) | b1;
    packet.payload_unit_start = (b0 >> 6) & 0x01;
    packet.adaptation_field_control = (b2 >> 4) & 0x03;
    packet.continuity_counter = b2 & 0x0F;

    packet.has_adaptation_field = (packet.adaptation_field_control == AF_CONTROL_ADAPTATION_ONLY) ||
                                   (packet.adaptation_field_control == AF_CONTROL_BOTH);
    packet.has_payload = (packet.adaptation_field_control == AF_CONTROL_PAYLOAD_ONLY) ||
                          (packet.adaptation_field_control == AF_CONTROL_BOTH);

    // Parse adaptation field if present
    packet.pcr = -1;
    packet.has_pcr = false;

    if (packet.has_adaptation_field) {
        int offset = TS_HEADER_SIZE;
        uint8_t adap_len = data[offset];
        offset++;

        if (adap_len > 0) {
            uint8_t flags = data[offset];
            bool pcr_flag = (flags >> 4) & 0x01;
            (void)pcr_flag;  // Suppress unused variable warning

            if (pcr_flag && adap_len >= 7) {
                // Parse PCR (48 bits + 6-bit extension)
                uint8_t pcr_base_h = data[offset + 1];
                uint8_t pcr_base_m1 = data[offset + 2];
                uint8_t pcr_base_m2 = data[offset + 3];
                uint8_t pcr_base_l = data[offset + 4];
                uint8_t pcr_ext_h = data[offset + 5];
                // uint8_t pcr_ext_l = data[offset + 6];

                uint64_t pcr_base = ((uint64_t)pcr_base_h << 25) |
                                     ((uint64_t)pcr_base_m1 << 17) |
                                     ((uint64_t)pcr_base_m2 << 9) |
                                     ((uint64_t)pcr_base_l << 1) |
                                     ((pcr_ext_h >> 7) & 0x01);
                uint16_t pcr_ext = ((pcr_ext_h & 0x01) << 8) | data[offset + 6];

                packet.pcr = (int64_t)(pcr_base * 300 + pcr_ext);
                packet.has_pcr = true;
            }
        }

        // Payload starts after adaptation field
        packet.payload = data + TS_HEADER_SIZE + 1 + adap_len;
        packet.payload_size = TS_PACKET_SIZE - TS_HEADER_SIZE - 1 - adap_len;
    } else {
        packet.payload = data + TS_HEADER_SIZE;
        packet.payload_size = TS_PACKET_SIZE - TS_HEADER_SIZE;
    }

    return packet.has_payload && packet.payload_size > 0;
}

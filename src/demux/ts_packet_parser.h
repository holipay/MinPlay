#pragma once
#include <cstdint>
#include <vector>

// TS packet constants
static constexpr int TS_PACKET_SIZE = 188;
static constexpr uint8_t TS_SYNC_BYTE = 0x47;
static constexpr int TS_HEADER_SIZE = 4;
static constexpr int TS_MAX_PAYLOAD_SIZE = 184;

// Adaptation field control values
static constexpr uint8_t AF_CONTROL_PAYLOAD_ONLY = 0x01;
static constexpr uint8_t AF_CONTROL_ADAPTATION_ONLY = 0x02;
static constexpr uint8_t AF_CONTROL_BOTH = 0x03;

struct TsPacket {
    uint16_t pid;                    // Packet Identifier
    uint8_t  payload_unit_start;     // Payload unit start indicator
    uint8_t  adaptation_field_control; // Adaptation field control
    uint8_t  continuity_counter;     // Continuity counter
    bool     has_adaptation_field;   // Has adaptation field
    bool     has_payload;            // Has payload
    int64_t  pcr;                    // Program Clock Reference (if present)
    bool     has_pcr;                // PCR present flag
    const uint8_t* payload;          // Pointer to payload data
    int      payload_size;           // Size of payload data
};

class TsPacketParser {
public:
    // Parse a single 188-byte TS packet
    bool ParsePacket(const uint8_t* data, int size, TsPacket& packet);

    // Find next sync byte in data stream
    // Returns offset of sync byte, or -1 if not found
    int FindSyncByte(const uint8_t* data, int size, int start_offset = 0);

    // Reset parser state
    void Reset() {}
};

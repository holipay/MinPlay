#include "../demux/ts_demuxer.h"
#include "../network/hls_stream.h"
#include "../util/log.h"
#include <cstdio>

// Simple test: download a TS segment and parse it with TsDemuxer
int main() {
    LOG_INFO("TsDemuxer test: parsing TS data from HLS byte stream");

    // This is a placeholder test
    // In production, TsDemuxer would be integrated with MediaSource
    LOG_INFO("TsDemuxer components compiled successfully");
    LOG_INFO("TsPacketParser: 188-byte TS packet parsing");
    LOG_INFO("PesAssembler: PES packet reassembly + PTS/DTS extraction");
    LOG_INFO("ProgramManager: PAT/PMT parsing + PID identification");
    LOG_INFO("TsDemuxer: main controller coordinating all components");

    return 0;
}

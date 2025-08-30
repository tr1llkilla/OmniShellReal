Copyright © 2025 Cadell Richard Anderson

//packet_reader.h

#pragma once
#include "packet_frame.h"
#include <string>

namespace ironrouter {

    class InProcessPacketReaderHelper {
    public:
        InProcessPacketReaderHelper() = default;

        // Reads one frame (blocking).
        bool read_one(PacketFrame& frame, const std::string& ringName = "uplink");

        // Reads one frame (non-blocking).
        bool try_read_one(PacketFrame& frame, const std::string& ringName = "uplink");
    };

} // namespace ironrouter

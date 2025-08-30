//packet_reader.h

#pragma once
#include "packet_frame.h"
#include <string>

namespace ironrouter {

    // Renamed for clarity and consistency with the InProcessPacketWriter/Reader classes.
    class InProcessPacketReaderHelper {
    public:
        InProcessPacketReaderHelper() = default;

        // Reads one frame (blocking).
        bool read_one(PacketFrame& frame, const std::string& ringName = "uplink");

        // Reads one frame (non-blocking).
        bool try_read_one(PacketFrame& frame, const std::string& ringName = "uplink");
    };

} // namespace ironrouter
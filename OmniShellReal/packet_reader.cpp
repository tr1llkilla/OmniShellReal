Copyright © 2025 Cadell Richard Anderson

//packet_reader.cpp

#include "packet_reader.h"
#include "packet_frame.h" // Needed for make_in_process_packet_reader and PacketFrame

namespace ironrouter {

    bool InProcessPacketReaderHelper::read_one(PacketFrame& frame, const std::string& ringName) {
        // Updated to call the renamed in-process factory function.
        auto reader = make_in_process_packet_reader(ringName);
        if (!reader) return false;
        return reader->read(frame);
    }

    bool InProcessPacketReaderHelper::try_read_one(PacketFrame& frame, const std::string& ringName) {
        // Updated to call the renamed in-process factory function.
        auto reader = make_in_process_packet_reader(ringName);
        if (!reader) return false;
        return reader->try_read(frame);
    }

} // namespace ironrouter

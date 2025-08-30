Copyright © 2025 Cadell Richard Anderson

//ap_pcap_listener.h

#pragma once
#include "types.h" // For PcapRecordHeader, u8, u16, and other pcap structs
#include <string>
#include <functional>
#include <memory>

namespace ironrouter {

    // FrameCallback now uses the u8 type alias for consistency.
    using FrameCallback =
        std::function<void(const u8* data, size_t len, const PcapRecordHeader& hdr)>;

    class ApPcapListener {
    public:
        ApPcapListener();
        ~ApPcapListener();

        ApPcapListener(const ApPcapListener&) = delete;
        ApPcapListener& operator=(const ApPcapListener&) = delete;

        // The 'port' parameter now uses the u16 type alias.
        bool start(int deviceID,
            u16 port,
            const std::string& outBase = "ap_stream",
            bool fileSink = true,
            bool verbose = false,
            const std::string& filter_expression = "");

        void stop();
        void set_frame_callback(FrameCallback cb);
        void set_inject_adapter(int adapterIndex);

    private:
        struct Impl;
        Impl* pimpl;
    };

} // namespace ironrouter

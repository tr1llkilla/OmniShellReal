Copyright © 2025 Cadell Richard Anderson

// live_capture.h

#pragma once
#include "types.h" // For PcapRecordHeader, u8, etc.
#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace ironrouter {

    // Describes a network device available for capture.
    struct NetworkDevice {
        int id;
        std::string name;
        std::string description;
    };

    // Callback function type for handling captured frames.
    using FrameCallback =
        std::function<void(const u8* data, size_t len, const PcapRecordHeader& hdr)>;

    // Manages a live packet capture session on a background thread.
    class LiveCapture {
    public:
        LiveCapture();
        ~LiveCapture();

        // Non-copyable and non-movable
        LiveCapture(const LiveCapture&) = delete;
        LiveCapture& operator=(const LiveCapture&) = delete;

        // Lists all available network devices for capture.
        static std::vector<NetworkDevice> list_devices();

        // Starts the capture on a specific device.
        bool start_capture(int device_index, FrameCallback callback, const std::string& filter_expression = "");

        // Stops the capture if it is running.
        void stop_capture();

    private:
        struct Impl;
        std::unique_ptr<Impl> pimpl;
    };

} // namespace ironrouter

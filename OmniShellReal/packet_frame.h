//packet_frame.h

#pragma once
#include "types.h" // For u8, u32 typedefs
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ironrouter {

    struct PacketFrame {
        std::chrono::system_clock::time_point ts;
        std::vector<u8>                  data;
        u32                              caplen{ 0 };
        u32                              origlen{ 0 };
    };

    // In-process bounded ring buffer
    class PacketRingBuffer {
    public:
        explicit PacketRingBuffer(size_t capacity);
        void push(PacketFrame&& frame);
        bool pop(PacketFrame& out);
        bool try_pop(PacketFrame& out);
        size_t size() const;

    private:
        const size_t capacity_;
        mutable std::mutex mtx_;
        std::condition_variable cv_;
        std::deque<PacketFrame> queue_;
    };

    // Renamed to avoid conflict with IPC PacketWriter
    class InProcessPacketWriter {
    public:
        explicit InProcessPacketWriter(std::shared_ptr<PacketRingBuffer> buf) : buf_(std::move(buf)) {}
        void write(PacketFrame&& f) { buf_->push(std::move(f)); }
        std::shared_ptr<PacketRingBuffer> buffer() const { return buf_; }
    private:
        std::shared_ptr<PacketRingBuffer> buf_;
    };

    // Renamed to avoid conflict with a potential IPC PacketReader
    class InProcessPacketReader {
    public:
        explicit InProcessPacketReader(std::shared_ptr<PacketRingBuffer> buf) : buf_(std::move(buf)) {}
        bool read(PacketFrame& out) { return buf_->pop(out); }
        bool try_read(PacketFrame& out) { return buf_->try_pop(out); }
        std::shared_ptr<PacketRingBuffer> buffer() const { return buf_; }
    private:
        std::shared_ptr<PacketRingBuffer> buf_;
    };

    // Globals for in-process packet rings - updated to use renamed class
    extern std::map<std::string, std::shared_ptr<InProcessPacketWriter>> g_packet_writers;
    extern std::shared_ptr<PacketRingBuffer> g_uplink_buf;

    // Functions updated to use new names and types
    void register_packet_rings();
    std::shared_ptr<InProcessPacketWriter> get_or_create_in_process_writer(const std::string& name,
        size_t capacity = 1024);
    std::shared_ptr<InProcessPacketWriter> get_uplink_writer();
    std::shared_ptr<InProcessPacketReader> make_in_process_packet_reader(const std::string& name);

    bool queue_bytes_to_uplink(std::vector<u8>&& bytes);
    bool queue_bytes_to_uplink(const u8* data, size_t len);

} // namespace ironrouter
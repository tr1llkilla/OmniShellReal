//packet_frame.cpp

#include "packet_frame.h"

namespace ironrouter {

    PacketRingBuffer::PacketRingBuffer(size_t capacity)
        : capacity_(capacity ? capacity : 1) {
    }

    void PacketRingBuffer::push(PacketFrame&& frame) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (queue_.size() == capacity_) {
                queue_.pop_front(); // drop oldest
            }
            queue_.push_back(std::move(frame));
        }
        cv_.notify_one();
    }

    bool PacketRingBuffer::pop(PacketFrame& out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [&] { return !queue_.empty(); });
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    bool PacketRingBuffer::try_pop(PacketFrame& out) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    size_t PacketRingBuffer::size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    // ---------------------------
    // Globals
    // ---------------------------
    // Updated to use renamed class
    std::map<std::string, std::shared_ptr<InProcessPacketWriter>> g_packet_writers;
    std::shared_ptr<PacketRingBuffer> g_uplink_buf;

    void register_packet_rings() {
        if (!g_uplink_buf) {
            g_uplink_buf = std::make_shared<PacketRingBuffer>(1024);
        }
        // Updated to use renamed class
        g_packet_writers["uplink"] = std::make_shared<InProcessPacketWriter>(g_uplink_buf);
    }

    // Renamed function and updated to use InProcessPacketWriter
    std::shared_ptr<InProcessPacketWriter> get_or_create_in_process_writer(const std::string& name,
        size_t capacity) {
        auto it = g_packet_writers.find(name);
        if (it != g_packet_writers.end()) return it->second;

        auto buf = std::make_shared<PacketRingBuffer>(capacity);
        auto wr = std::make_shared<InProcessPacketWriter>(buf);
        g_packet_writers[name] = wr;

        if (name == "uplink") g_uplink_buf = buf;
        return wr;
    }

    // Updated to return renamed class type
    std::shared_ptr<InProcessPacketWriter> get_uplink_writer() {
        auto it = g_packet_writers.find("uplink");
        if (it != g_packet_writers.end()) return it->second;
        return nullptr;
    }

    // Renamed function and updated to use and return InProcessPacketReader
    std::shared_ptr<InProcessPacketReader> make_in_process_packet_reader(const std::string& name) {
        auto it = g_packet_writers.find(name);
        if (it == g_packet_writers.end()) return nullptr;
        return std::make_shared<InProcessPacketReader>(it->second->buffer());
    }

    bool queue_bytes_to_uplink(std::vector<u8>&& bytes) {
        if (!g_uplink_buf) return false;
        PacketFrame f;
        f.ts = std::chrono::system_clock::now();
        f.caplen = f.origlen = static_cast<u32>(bytes.size());
        f.data = std::move(bytes);
        g_uplink_buf->push(std::move(f));
        return true;
    }

    bool queue_bytes_to_uplink(const u8* data, size_t len) {
        if (!g_uplink_buf) return false;
        PacketFrame f;
        f.ts = std::chrono::system_clock::now();
        f.caplen = f.origlen = static_cast<u32>(len);
        f.data.assign(data, data + len);
        g_uplink_buf->push(std::move(f));
        return true;
    }

} // namespace ironrouter
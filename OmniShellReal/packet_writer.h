
// packet_writer.h
#pragma once
// Provides a userland-backed, zero-copy ring buffer using shared memory on Windows.
// This is suitable for high-throughput, low-latency inter-process communication.
#include "types.h"
#include <windows.h> // For HANDLE type

namespace ironrouter::ipc {

    // The control block for the shared memory ring buffer.
    // This structure is placed at the beginning of the shared memory region.
#pragma pack(push,1)
    struct RingControl {
        // The index of the next block to be written by the producer. Atomically updated.
        alignas(64) volatile u64 producer_index;
        // The index of the next block to be read by the consumer. Atomically updated.
        alignas(64) volatile u64 consumer_index;
        u64 block_bytes;    // The size of each data block in bytes.
        u64 blocks;         // The total number of blocks in the ring.
        u64 sample_base;    // Optional: Base timestamp or sample counter for the stream.
        u8  reserved[4096 - (2 * sizeof(u64) + 3 * sizeof(u64))]; // Pad to 4KB alignment.
    };
#pragma pack(pop)

    // Manages a shared memory ring buffer for writing data.
    class PacketWriter {
    public:
        PacketWriter(const char* backingName, size_t block_bytes, size_t blocks);
        ~PacketWriter();

        // Non-copyable to prevent handle duplication.
        PacketWriter(const PacketWriter&) = delete;
        PacketWriter& operator=(const PacketWriter&) = delete;

        // Creates a new shared memory region or opens an existing one.
        bool open_or_create();

        // Acquires a pointer to the next available block for writing.
        void* acquire_block_ptr(u64& outBlockIndex);

        // Commits a written block, making it visible to consumers.
        void commit_produce();

        // Atomically reads the current producer and consumer indices.
        u64 query_producer_index();
        u64 query_consumer_index();

        // Allows a consumer to advance its index after reading.
        void advance_consumer(u64 newIndex);

        // Gets a direct pointer to a block by its absolute index.
        void* block_ptr(u64 blockIndex);

    private:
        char name[256];
        size_t blockBytes;
        size_t numBlocks;
        RingControl* ctrl;
        void* basePtr;
        HANDLE hMap;
    };

} // namespace ironrouter::ipc
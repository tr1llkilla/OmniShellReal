Copyright © 2025 Cadell Richard Anderson

// packet_writer.cpp

#include "packet_writer.h"
#include <iostream>
#include <cassert>
#include <cstring>

namespace ironrouter::ipc {

    PacketWriter::PacketWriter(const char* backingName, size_t block_bytes, size_t blocks) :
        blockBytes(block_bytes), numBlocks(blocks), ctrl(nullptr), basePtr(nullptr), hMap(nullptr)
    {
        strncpy_s(name, sizeof(name), backingName, _TRUNCATE);
    }

    PacketWriter::~PacketWriter() {
        if (ctrl) UnmapViewOfFile(ctrl);
        if (hMap) CloseHandle(hMap);
    }

    bool PacketWriter::open_or_create() {
        const size_t controlSize = sizeof(RingControl);
        // Ensure the control block is exactly 4KB for page alignment.
        assert(controlSize == 4096 && "RingControl size must be 4KB");
        const size_t totalSize = controlSize + blockBytes * numBlocks;

        hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)totalSize, name);
        if (!hMap) {
            std::cerr << "[PacketWriter] CreateFileMapping failed with error: " << GetLastError() << "\n";
            return false;
        }

        bool alreadyExists = (GetLastError() == ERROR_ALREADY_EXISTS);

        void* mapView = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, totalSize);
        if (!mapView) {
            std::cerr << "[PacketWriter] MapViewOfFile failed with error: " << GetLastError() << "\n";
            CloseHandle(hMap);
            hMap = nullptr;
            return false;
        }

        ctrl = static_cast<RingControl*>(mapView);
        basePtr = static_cast<uint8_t*>(mapView) + controlSize;

        if (!alreadyExists) {
            // This is a new mapping, so initialize the control block.
            ctrl->producer_index = 0;
            ctrl->consumer_index = 0;
            ctrl->block_bytes = (u64)blockBytes;
            ctrl->blocks = (u64)numBlocks;
            ctrl->sample_base = 0;
            // It's good practice to zero the data area.
            memset(basePtr, 0, blockBytes * numBlocks);
        }
        else {
            // Mapping already existed, validate its parameters.
            if (ctrl->block_bytes != blockBytes || ctrl->blocks != numBlocks) {
                std::cerr << "[PacketWriter] Warning: Existing ring '" << name
                    << "' has different dimensions. Using existing.\n";
                // Update local members to match the existing ring's properties.
                this->blockBytes = ctrl->block_bytes;
                this->numBlocks = ctrl->blocks;
            }
        }
        return true;
    }

    void* PacketWriter::acquire_block_ptr(u64& outBlockIndex) {
        // Atomically read producer and consumer indices.
        u64 prod = InterlockedCompareExchange64((volatile LONG64*)&ctrl->producer_index, 0, 0);
        u64 cons = InterlockedCompareExchange64((volatile LONG64*)&ctrl->consumer_index, 0, 0);

        // Check if the ring buffer is full.
        if (prod - cons >= numBlocks) {
            return nullptr;
        }
        outBlockIndex = prod;
        return block_ptr(prod);
    }

    void PacketWriter::commit_produce() {
        // Atomically increment the producer index to publish the new block.
        InterlockedIncrement64((volatile LONG64*)&ctrl->producer_index);
    }

    u64 PacketWriter::query_producer_index() {
        return InterlockedCompareExchange64((volatile LONG64*)&ctrl->producer_index, 0, 0);
    }

    u64 PacketWriter::query_consumer_index() {
        return InterlockedCompareExchange64((volatile LONG64*)&ctrl->consumer_index, 0, 0);
    }

    void PacketWriter::advance_consumer(u64 newIndex) {
        // Atomically set the consumer index.
        InterlockedExchange64((volatile LONG64*)&ctrl->consumer_index, (LONG64)newIndex);
    }

    void* PacketWriter::block_ptr(u64 blockIndex) {
        // Calculate the position of the block within the ring.
        u64 idx_in_ring = blockIndex % numBlocks;
        return static_cast<uint8_t*>(basePtr) + (idx_in_ring * blockBytes);
    }

} // namespace ironrouter::ipc

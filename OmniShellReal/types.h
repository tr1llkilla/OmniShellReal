Copyright © 2025 Cadell Richard Anderson

// types.h

#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <string_view>
#include <fstream>
#include <iostream>
#include <limits>
#include <cmath>
#include <random>
#include <algorithm>
#include <cassert>

// Existing aliases
using f32 = float;
using f64 = double;
using i16 = int16_t;
using i32 = int32_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using u8 = uint8_t;

// ==== NEW: Stronger semantic typedefs for AI pipelines ====
using token_id = i32;
using vocab_size = u32;

// Shape descriptor for tensors
struct Shape {
    std::vector<size_t> dims;
    size_t numel() const {
        size_t total = 1;
        for (auto d : dims) total *= d;
        return total;
    }
    bool operator==(const Shape& other) const { return dims == other.dims; }
};

// Tensor metadata
struct TensorInfo {
    Shape shape;
    std::string dtype; // "f32", "f16", "q4_0", etc.
};

// Generation/sampling configuration
struct SamplingParams {
    f32 temperature = 0.8f;
    i32 top_k = 40;
    f32 top_p = 0.95f;
    f32 repetition_penalty = 1.1f;
    bool do_sample = true;

    // ==== ADDITION: minimum probability threshold ====
    f32 min_prob = 0.0f;

    bool has_min_prob() const noexcept { return min_prob > 0.0f; }
};

// Simple checked read (existing)
template <typename T>
inline void read_or_die(std::ifstream& in, T* dst, size_t count = 1) {
    in.read(reinterpret_cast<char*>(dst), sizeof(T) * count);
    // CORRECTED: Changed '| |' to the correct logical OR operator '||'
    if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(T) * count)) {
        std::cerr << "Fatal: Failed to read " << (sizeof(T) * count) << " bytes\n";
        std::abort();
    }
}

// Simple span view over contiguous f32 memory (existing)
struct Span {
    f32* data = nullptr;
    size_t size = 0;

    bool empty() const { return size == 0; }
    // CORRECTED: Changed operator() to the correct subscript operator
    f32& operator[](size_t idx) { return data[idx]; }
    const f32& operator[](size_t idx) const { return data[idx]; }
};

// =========================================================
// PCAP record and global header types
// =========================================================
namespace ironrouter {

#pragma pack(push,1)
    struct PcapRecordHeader {
        u32 ts_sec;
        u32 ts_usec;
        u32 incl_len;
        u32 orig_len;
    };

    struct pcap_hdr_t {
        u32 magic_number;
        u16 version_major;
        u16 version_minor;
        i32 thiszone;
        u32 sigfigs;
        u32 snaplen;
        u32 network;
    };

    struct pcaprec_hdr_t {
        u32 ts_sec;
        u32 ts_usec;
        u32 incl_len;
        u32 orig_len;
    };
#pragma pack(pop)

} // namespace ironrouter

// =========================================================
// NEW: Cloud Container Manifest Types
// =========================================================
namespace onecloud {

    // Represents a single chunk of file data within the container.
    struct DataChunk {
        uint64_t offset_in_container;
        uint32_t compressed_size;
        uint32_t original_size;
    };

    // Represents a single virtual file or directory within the container.
    struct FileEntry {
        std::string path;
        uint64_t original_size = 0;
        int64_t creation_time = 0;
        int64_t last_write_time = 0;
        std::vector<DataChunk> chunks;
    };

    // Represents the entire in-memory manifest.
    struct Manifest {
        uint32_t version = 1;
        std::vector<FileEntry> files;
    };

} // namespace onecloud

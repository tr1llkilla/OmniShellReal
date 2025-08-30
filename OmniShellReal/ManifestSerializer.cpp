Copyright © 2025 Cadell Richard Anderson

// ManifestSerializer.cpp

#include "ManifestSerializer.h"
#include <cstring> // For memcpy
#include <type_traits> // For std::is_trivial_v

namespace onecloud {

    namespace {
        // Helper class for writing data to a byte buffer in a structured way.
        class BufferWriter {
        public:
            BufferWriter(std::vector<std::byte>& buffer) : buffer_(buffer) {}

            template<typename T>
            void write(const T& value) {
                static_assert(std::is_trivial_v<T>, "Can only write trivial types directly.");
                const auto* ptr = reinterpret_cast<const std::byte*>(&value);
                buffer_.insert(buffer_.end(), ptr, ptr + sizeof(T));
            }

            void write(const std::string& str) {
                write<uint32_t>(static_cast<uint32_t>(str.length()));
                const auto* ptr = reinterpret_cast<const std::byte*>(str.data());
                buffer_.insert(buffer_.end(), ptr, ptr + str.length());
            }
        private:
            std::vector<std::byte>& buffer_;
        };

        // Helper class for safely reading data from a byte buffer.
        class BufferReader {
        public:
            BufferReader(std::span<const std::byte> buffer) : view_(buffer) {}

            template<typename T>
            bool read(T& value) {
                static_assert(std::is_trivial_v<T>, "Can only read trivial types directly.");
                if (offset_ + sizeof(T) > view_.size()) return false;
                std::memcpy(&value, view_.data() + offset_, sizeof(T));
                offset_ += sizeof(T);
                return true;
            }

            bool read(std::string& str) {
                uint32_t len;
                if (!read(len)) return false;
                if (offset_ + len > view_.size()) return false;
                str.resize(len);
                std::memcpy(str.data(), view_.data() + offset_, len);
                offset_ += len;
                return true;
            }
        private:
            std::span<const std::byte> view_;
            size_t offset_ = 0;
        };
    }

    void ManifestSerializer::serialize(const Manifest& manifest, std::vector<std::byte>& out_buffer) {
        out_buffer.clear();
        BufferWriter writer(out_buffer);

        // --- Manifest Header ---
        writer.write<uint32_t>(manifest.version);
        writer.write<uint32_t>(static_cast<uint32_t>(manifest.files.size()));

        // --- File Entries ---
        for (const auto& file : manifest.files) {
            writer.write(file.path);
            writer.write(file.original_size);
            writer.write(file.creation_time);
            writer.write(file.last_write_time);

            // --- Chunks ---
            writer.write<uint32_t>(static_cast<uint32_t>(file.chunks.size()));
            for (const auto& chunk : file.chunks) {
                writer.write(chunk.offset_in_container);
                writer.write(chunk.compressed_size);
                writer.write(chunk.original_size);
            }
        }
    }

    bool ManifestSerializer::deserialize(std::span<const std::byte> buffer, Manifest& out_manifest) {
        out_manifest = {}; // Clear the output manifest
        BufferReader reader(buffer);

        // --- Manifest Header ---
        if (!reader.read(out_manifest.version)) return false;
        if (out_manifest.version != 1) return false; // Only support version 1 for now

        uint32_t file_count;
        if (!reader.read(file_count)) return false;
        out_manifest.files.resize(file_count);

        // --- File Entries ---
        for (uint32_t i = 0; i < file_count; ++i) {
            auto& file = out_manifest.files[i];
            if (!reader.read(file.path)) return false;
            if (!reader.read(file.original_size)) return false;
            if (!reader.read(file.creation_time)) return false;
            if (!reader.read(file.last_write_time)) return false;

            // --- Chunks ---
            uint32_t chunk_count;
            if (!reader.read(chunk_count)) return false;
            file.chunks.resize(chunk_count);
            for (uint32_t j = 0; j < chunk_count; ++j) {
                auto& chunk = file.chunks[j];
                if (!reader.read(chunk.offset_in_container)) return false;
                if (!reader.read(chunk.compressed_size)) return false;
                if (!reader.read(chunk.original_size)) return false;
            }
        }
        return true;
    }

} // namespace onecloud

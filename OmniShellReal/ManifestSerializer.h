Copyright © 2025 Cadell Richard Anderson
//ManifestSerializer.h

#pragma once

#include "types.h" 
#include <span>
#include <vector>
#include <cstddef>

namespace onecloud {

    /**
     * @class ManifestSerializer
     * @brief Provides static methods to serialize and deserialize the manifest
     *        to and from a custom, high-performance binary format.
     *
     * This class replaces the dependency on external libraries like FlatBuffers,
     * offering a self-contained and build-friendly solution for manifest management.
     */
    class ManifestSerializer {
    public:
        /**
         * @brief Serializes an in-memory Manifest object into a byte buffer.
         * @param manifest The Manifest object to serialize.
         * @param out_buffer The byte vector that will be cleared and filled with the serialized data.
         */
        static void serialize(const Manifest& manifest, std::vector<std::byte>& out_buffer);

        /**
         * @brief Deserializes a byte buffer into an in-memory Manifest object.
         * @param buffer A span representing the byte buffer to deserialize.
         * @param out_manifest The Manifest object that will be populated with the deserialized data.
         * @return True if deserialization is successful, false otherwise (e.g., due to corrupted
         *         data, unsupported version, or insufficient buffer size).
         */
        static bool deserialize(std::span<const std::byte> buffer, Manifest& out_manifest);
    };

} // namespace onecloud

Copyright © 2025 Cadell Richard Anderson

#include "CloudStorage.h"
#include "ManifestSerializer.h"
#include "CryptoProvider.h"
#include <zstd.h>
#include <fstream>
#include <vector>
#include <map>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <expected>

namespace onecloud {

    // --- Constants and Helper Structs ---
    namespace {
        constexpr uint32_t OCV_MAGIC_NUMBER = 0x4F435632; // "OCV2"
        constexpr uint32_t OCV_FORMAT_VERSION = 1;
        constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024; // 4 MB chunks

#pragma pack(push, 1)
        struct ContainerHeader {
            uint32_t magic_number;
            uint32_t format_version;
            uint64_t manifest_offset;
            uint64_t manifest_length;
            uint64_t flags;
            unsigned char pwhash_salt[CryptoProvider::salt_length()];
        };
#pragma pack(pop)
    }

    // The actual implementation details are hidden here
    class CloudStorage::Impl {
    public:
        std::filesystem::path containerPath;
        std::vector<std::byte> masterKey;
        std::map<std::string, onecloud::FileEntry> manifestCache;

        std::expected<void, CloudError> loadManifest();
        std::expected<void, CloudError> saveManifest();
    };

    // --- CloudStorage Public Implementation ---

    CloudStorage::CloudStorage() : pImpl(std::make_unique<Impl>()) {}
    CloudStorage::~CloudStorage() = default;
    CloudStorage::CloudStorage(CloudStorage&&) noexcept = default;
    CloudStorage& CloudStorage::operator=(CloudStorage&&) noexcept = default;

    std::expected<CloudStorage, CloudError> CloudStorage::create(const std::filesystem::path& path, const std::string& password) {
        if (std::filesystem::exists(path)) {
            return std::unexpected(CloudError::FileExists);
        }

        try {
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
        }
        catch (const std::filesystem::filesystem_error&) {
            return std::unexpected(CloudError::IOError);
        }

        CloudStorage storage;
        storage.pImpl->containerPath = path;

        ContainerHeader header{};
        header.magic_number = OCV_MAGIC_NUMBER;
        header.format_version = OCV_FORMAT_VERSION;
        header.manifest_offset = 0;
        header.manifest_length = 0;
        header.flags = 0;

        auto salt = CryptoProvider::random_bytes(CryptoProvider::salt_length());
        if (salt.size() != CryptoProvider::salt_length()) {
            return std::unexpected(CloudError::EncryptionFailed);
        }
        std::memcpy(header.pwhash_salt, salt.data(), salt.size());

        auto keyResult = CryptoProvider::derive_key_from_password(password, salt);
        if (!keyResult) return std::unexpected(keyResult.error());
        storage.pImpl->masterKey = std::move(*keyResult);

        std::ofstream file(path, std::ios::binary);
        if (!file) return std::unexpected(CloudError::IOError);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.close();

        auto saveResult = storage.pImpl->saveManifest();
        if (!saveResult) return std::unexpected(saveResult.error());

        return storage;
    }

    std::expected<CloudStorage, CloudError> CloudStorage::open(const std::filesystem::path& path, const std::string& password) {
        if (!std::filesystem::exists(path)) {
            return std::unexpected(CloudError::ContainerNotFound);
        }

        std::ifstream file(path, std::ios::binary);
        if (!file) return std::unexpected(CloudError::IOError);

        ContainerHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (header.magic_number != OCV_MAGIC_NUMBER) {
            return std::unexpected(CloudError::InvalidContainerFormat);
        }

        CloudStorage storage;
        storage.pImpl->containerPath = path;

        std::vector<std::byte> salt(reinterpret_cast<const std::byte*>(header.pwhash_salt), reinterpret_cast<const std::byte*>(header.pwhash_salt) + sizeof(header.pwhash_salt));
        auto keyResult = CryptoProvider::derive_key_from_password(password, salt);
        if (!keyResult) return std::unexpected(keyResult.error());
        storage.pImpl->masterKey = std::move(*keyResult);

        auto loadResult = storage.pImpl->loadManifest();
        if (!loadResult) {
            return std::unexpected(loadResult.error());
        }

        return storage;
    }

    // --- PIMPL Method Implementations ---

    std::expected<void, CloudError> CloudStorage::Impl::loadManifest() {
        std::ifstream file(containerPath, std::ios::binary);
        if (!file) return std::unexpected(CloudError::IOError);

        ContainerHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (header.magic_number != OCV_MAGIC_NUMBER) return std::unexpected(CloudError::InvalidContainerFormat);

        if (header.manifest_offset == 0 || header.manifest_length == 0) {
            manifestCache.clear();
            return {};
        }

        file.seekg(header.manifest_offset);
        std::vector<std::byte> encrypted_buffer(header.manifest_length);
        file.read(reinterpret_cast<char*>(encrypted_buffer.data()), encrypted_buffer.size());

        auto decryptResult = CryptoProvider::decrypt(encrypted_buffer, masterKey);
        if (!decryptResult) return std::unexpected(decryptResult.error());

        auto& compressed_buffer = *decryptResult;
        unsigned long long decompressed_size = ZSTD_getFrameContentSize(compressed_buffer.data(), compressed_buffer.size());
        if (decompressed_size == ZSTD_CONTENTSIZE_ERROR || decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            return std::unexpected(CloudError::InvalidContainerFormat);
        }

        std::vector<std::byte> manifest_buffer(decompressed_size);
        ZSTD_decompress(manifest_buffer.data(), manifest_buffer.size(), compressed_buffer.data(), compressed_buffer.size());

        onecloud::Manifest manifest_data;
        if (!ManifestSerializer::deserialize(manifest_buffer, manifest_data)) {
            return std::unexpected(CloudError::InvalidContainerFormat);
        }

        manifestCache.clear();
        for (auto& file_entry : manifest_data.files) {
            manifestCache[file_entry.path] = std::move(file_entry);
        }

        return {};
    }

    std::expected<void, CloudError> CloudStorage::Impl::saveManifest() {
        onecloud::Manifest manifest_to_save;
        for (const auto& pair : manifestCache) {
            manifest_to_save.files.push_back(pair.second);
        }

        std::vector<std::byte> manifest_buffer;
        ManifestSerializer::serialize(manifest_to_save, manifest_buffer);

        size_t compressed_bound = ZSTD_compressBound(manifest_buffer.size());
        std::vector<std::byte> compressed_buffer(compressed_bound);
        size_t compressed_size = ZSTD_compress(compressed_buffer.data(), compressed_bound, manifest_buffer.data(), manifest_buffer.size(), 3);
        if (ZSTD_isError(compressed_size)) return std::unexpected(CloudError::IOError);
        compressed_buffer.resize(compressed_size);

        auto encryptResult = CryptoProvider::encrypt(compressed_buffer, masterKey);
        if (!encryptResult) return std::unexpected(encryptResult.error());
        auto& encrypted_buffer = *encryptResult;

        std::fstream file(containerPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!file) return std::unexpected(CloudError::IOError);

        file.seekp(0, std::ios::end);
        uint64_t new_manifest_offset = static_cast<uint64_t>(file.tellp());
        uint64_t new_manifest_length = encrypted_buffer.size();

        file.write(reinterpret_cast<const char*>(encrypted_buffer.data()), encrypted_buffer.size());

        file.seekp(offsetof(ContainerHeader, manifest_offset));
        file.write(reinterpret_cast<const char*>(&new_manifest_offset), sizeof(new_manifest_offset));
        file.write(reinterpret_cast<const char*>(&new_manifest_length), sizeof(new_manifest_length));

        return {};
    }

    // --- Public Method Implementations ---

    std::expected<std::vector<std::byte>, CloudError> CloudStorage::readFile(const std::string& virtual_path) {
        auto it = pImpl->manifestCache.find(virtual_path);
        if (it == pImpl->manifestCache.end()) {
            return std::unexpected(CloudError::FileNotFound);
        }

        const auto& file_entry = it->second;
        std::vector<std::byte> full_data(file_entry.original_size);
        std::byte* current_pos = full_data.data();

        std::ifstream file(pImpl->containerPath, std::ios::binary);
        if (!file) return std::unexpected(CloudError::IOError);

        for (const auto& chunk : file_entry.chunks) {
            file.seekg(chunk.offset_in_container);
            std::vector<std::byte> encrypted_chunk(chunk.compressed_size);
            file.read(reinterpret_cast<char*>(encrypted_chunk.data()), encrypted_chunk.size());

            auto decryptResult = CryptoProvider::decrypt(encrypted_chunk, pImpl->masterKey);
            if (!decryptResult) return std::unexpected(decryptResult.error());

            auto& compressed_chunk = *decryptResult;
            ZSTD_decompress(current_pos, chunk.original_size, compressed_chunk.data(), compressed_chunk.size());
            current_pos += chunk.original_size;
        }

        return full_data;
    }

    std::expected<void, CloudError> CloudStorage::writeFile(const std::string& virtual_path, std::span<const std::byte> data) {
        std::fstream file(pImpl->containerPath, std::ios::in | std::ios::out | std::ios::binary | std::ios::ate);
        if (!file) return std::unexpected(CloudError::IOError);

        // FIXED: Use value-initialization to prevent potential uninitialized members
        onecloud::FileEntry new_entry{};
        new_entry.path = virtual_path;
        new_entry.original_size = data.size();
        new_entry.creation_time = std::chrono::system_clock::now().time_since_epoch().count();
        new_entry.last_write_time = new_entry.creation_time;

        size_t bytes_processed = 0;
        while (bytes_processed < data.size()) {
            size_t current_chunk_size = std::min(CHUNK_SIZE, data.size() - bytes_processed);
            std::span<const std::byte> chunk_span = data.subspan(bytes_processed, current_chunk_size);

            size_t compressed_bound = ZSTD_compressBound(current_chunk_size);
            std::vector<std::byte> compressed_chunk(compressed_bound);
            size_t compressed_size = ZSTD_compress(compressed_chunk.data(), compressed_bound, chunk_span.data(), chunk_span.size(), 3);
            if (ZSTD_isError(compressed_size)) return std::unexpected(CloudError::IOError);
            compressed_chunk.resize(compressed_size);

            auto encryptResult = CryptoProvider::encrypt(compressed_chunk, pImpl->masterKey);
            if (!encryptResult) return std::unexpected(encryptResult.error());
            auto& encrypted_chunk = *encryptResult;

            uint64_t chunk_offset = static_cast<uint64_t>(file.tellp());
            file.write(reinterpret_cast<const char*>(encrypted_chunk.data()), encrypted_chunk.size());

            // FIXED: Use value-initialization to prevent potential uninitialized members
            onecloud::DataChunk chunk_metadata{};
            chunk_metadata.offset_in_container = chunk_offset;
            chunk_metadata.compressed_size = static_cast<uint32_t>(encrypted_chunk.size());
            chunk_metadata.original_size = static_cast<uint32_t>(current_chunk_size);
            new_entry.chunks.push_back(chunk_metadata);

            bytes_processed += current_chunk_size;
        }

        pImpl->manifestCache[virtual_path] = std::move(new_entry);
        return pImpl->saveManifest();
    }

    std::expected<void, CloudError> CloudStorage::deleteFile(const std::string& virtual_path) {
        auto it = pImpl->manifestCache.find(virtual_path);
        if (it == pImpl->manifestCache.end()) {
            return std::unexpected(CloudError::FileNotFound);
        }
        pImpl->manifestCache.erase(it);
        return pImpl->saveManifest();
    }

    std::expected<std::vector<std::string>, CloudError> CloudStorage::listFiles() {
        std::vector<std::string> file_list;
        file_list.reserve(pImpl->manifestCache.size());
        for (const auto& pair : pImpl->manifestCache) {
            file_list.push_back(pair.first);
        }
        return file_list;
    }

} // namespace onecloud

//CloudStorage.h

#pragma once

#include "CloudError.h" // CORRECTED: Include the new, centralized error header
#include <string>
#include <vector>
#include <memory>
#include <filesystem>
#include <span>
#include <expected>
#include <cstddef>

// NOTE: The local 'enum class CloudError' has been removed from this file.

namespace onecloud {

    // This is the real C++ class that performs all the work.
    class CloudStorage {
    public:
        // --- Static factory functions ---
        static std::expected<CloudStorage, onecloud::CloudError> create(const std::filesystem::path& path, const std::string& password);
        static std::expected<CloudStorage, onecloud::CloudError> open(const std::filesystem::path& path, const std::string& password);

        // --- Rule of 5 for PIMPL ---
        CloudStorage(CloudStorage&&) noexcept;
        CloudStorage& operator=(CloudStorage&&) noexcept;
        ~CloudStorage();

        // --- Public methods that will be called by the C API ---
        std::expected<std::vector<std::byte>, onecloud::CloudError> readFile(const std::string& virtual_path);
        std::expected<void, onecloud::CloudError> writeFile(const std::string& virtual_path, std::span<const std::byte> data);
        std::expected<void, onecloud::CloudError> deleteFile(const std::string& virtual_path);
        std::expected<std::vector<std::string>, onecloud::CloudError> listFiles();

    private:
        // --- Private constructor ---
        CloudStorage();

        // --- Pointer to implementation (PIMPL) ---
        class Impl;
        std::unique_ptr<Impl> pImpl;
    };

} // namespace onecloud
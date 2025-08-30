Copyright © 2025 Cadell Richard Anderson

// CloudAPI.h

#pragma once

#include "onecloud_c_api.h"
#include "CloudError.h" 
#include <string>
#include <vector>
#include <memory>
#include <expected>
#include <filesystem>
#include <span>

namespace onecloud {

    // This is the C++ friendly wrapper around the C API
    class CloudAPI {
    private:
        // Custom deleter for the unique_ptr to call the C close function
        struct HandleDeleter {
            void operator()(OneCloud_StorageHandle* h) const {
                if (h) {
                    onecloud_storage_close(h);
                }
            }
        };

        std::unique_ptr<OneCloud_StorageHandle, HandleDeleter> m_handle;

        // Private constructor to force use of factory functions
        explicit CloudAPI(OneCloud_StorageHandle* handle) : m_handle(handle) {}

    public:
        // --- Factory Functions ---
        static std::expected<CloudAPI, CloudError> create(const std::filesystem::path& path, const std::string& password) {
            OneCloud_StorageHandle* handle = nullptr;
            OneCloud_Error err = onecloud_storage_create(path.string().c_str(), password.c_str(), &handle);
            if (err == ONECLOUD_SUCCESS) {
                return CloudAPI(handle);
            }
            return std::unexpected(static_cast<CloudError>(err));
        }

        static std::expected<CloudAPI, CloudError> open(const std::filesystem::path& path, const std::string& password) {
            OneCloud_StorageHandle* handle = nullptr;
            OneCloud_Error err = onecloud_storage_open(path.string().c_str(), password.c_str(), &handle);
            if (err == ONECLOUD_SUCCESS) {
                return CloudAPI(handle);
            }
            return std::unexpected(static_cast<CloudError>(err));
        }

        // --- File Operations ---
        std::expected<std::vector<std::byte>, CloudError> read_file(const std::string& virtual_path) {
            uint8_t* data = nullptr;
            size_t size = 0;
            OneCloud_Error err = onecloud_storage_read_file(m_handle.get(), virtual_path.c_str(), &data, &size);
            if (err != ONECLOUD_SUCCESS) {
                return std::unexpected(static_cast<CloudError>(err));
            }

            // CORRECTED: Added reinterpret_cast to handle the type conversion from uint8_t* to std::byte*
            const auto* byte_ptr_begin = reinterpret_cast<const std::byte*>(data);
            const auto* byte_ptr_end = reinterpret_cast<const std::byte*>(data + size);
            std::vector<std::byte> result(byte_ptr_begin, byte_ptr_end);

            onecloud_free_data_buffer(data); // Free the buffer allocated by the C API
            return result;
        }

        std::expected<void, CloudError> write_file(const std::string& virtual_path, std::span<const std::byte> data) {
            OneCloud_Error err = onecloud_storage_write_file(m_handle.get(), virtual_path.c_str(), reinterpret_cast<const uint8_t*>(data.data()), data.size());
            if (err == ONECLOUD_SUCCESS) {
                return {};
            }
            return std::unexpected(static_cast<CloudError>(err));
        }

        std::expected<void, CloudError> delete_file(const std::string& virtual_path) {
            OneCloud_Error err = onecloud_storage_delete_file(m_handle.get(), virtual_path.c_str());
            if (err == ONECLOUD_SUCCESS) {
                return {};
            }
            return std::unexpected(static_cast<CloudError>(err));
        }

        std::expected<std::vector<std::string>, CloudError> list_files() const {
            char** list = nullptr;
            size_t count = 0;
            OneCloud_Error err = onecloud_storage_list_files(m_handle.get(), &list, &count);
            if (err != ONECLOUD_SUCCESS) {
                return std::unexpected(static_cast<CloudError>(err));
            }
            std::vector<std::string> result;
            result.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                result.emplace_back(list[i]);
            }
            onecloud_free_file_list(list, count); // Free the list allocated by the C API
            return result;
        }
    };

} // namespace onecloud

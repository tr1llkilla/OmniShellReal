#define ONECLOUD_EXPORTS // Define this before including the header to ensure dllexport is used

// --- C++ headers MUST be included before C API headers ---
#include "CloudStorage.h" // Internal C++ header
#include "CloudAPI.h"
#include <vector>
#include <string>
#include <memory>
#include <cstring> // For malloc, free, memcpy, strdup
#include <span>      // For std::span

#include "onecloud_c_api.h"

// Helper to convert C++ CloudError to C OneCloud_Error
// FIXED: The CloudError enum is inside the 'onecloud' namespace.
static OneCloud_Error to_c_error(onecloud::CloudError err) {
    return static_cast<OneCloud_Error>(err);
}

extern "C" {

    // --- Container Operations ---

    ONECLOUD_API OneCloud_Error onecloud_storage_create(const char* path, const char* password, OneCloud_StorageHandle** handle) {
        if (!path || !password || !handle) return ONECLOUD_ERROR_UNKNOWN;
        try {
            auto result = onecloud::CloudStorage::create(path, password);
            if (result) {
                *handle = reinterpret_cast<OneCloud_StorageHandle*>(new onecloud::CloudStorage(std::move(*result)));
                return ONECLOUD_SUCCESS;
            }
            return to_c_error(result.error());
        }
        catch (...) {
            return ONECLOUD_ERROR_UNKNOWN;
        }
    }

    ONECLOUD_API OneCloud_Error onecloud_storage_open(const char* path, const char* password, OneCloud_StorageHandle** handle) {
        if (!path || !password || !handle) return ONECLOUD_ERROR_UNKNOWN;
        try {
            auto result = onecloud::CloudStorage::open(path, password);
            if (result) {
                *handle = reinterpret_cast<OneCloud_StorageHandle*>(new onecloud::CloudStorage(std::move(*result)));
                return ONECLOUD_SUCCESS;
            }
            return to_c_error(result.error());
        }
        catch (...) {
            return ONECLOUD_ERROR_UNKNOWN;
        }
    }

    ONECLOUD_API void onecloud_storage_close(OneCloud_StorageHandle* handle) {
        delete reinterpret_cast<onecloud::CloudStorage*>(handle);
    }

    // --- File Operations ---

    ONECLOUD_API OneCloud_Error onecloud_storage_read_file(OneCloud_StorageHandle* handle, const char* virtual_path, uint8_t** out_data, size_t* out_size) {
        if (!handle || !virtual_path || !out_data || !out_size) return ONECLOUD_ERROR_UNKNOWN;
        auto storage = reinterpret_cast<onecloud::CloudStorage*>(handle);
        try {
            auto result = storage->readFile(virtual_path);
            if (result) {
                auto& vec = *result;
                *out_data = static_cast<uint8_t*>(std::malloc(vec.size()));
                if (!*out_data) return ONECLOUD_ERROR_OUT_OF_MEMORY;
                std::memcpy(*out_data, vec.data(), vec.size());
                *out_size = vec.size();
                return ONECLOUD_SUCCESS;
            }
            return to_c_error(result.error());
        }
        catch (...) {
            return ONECLOUD_ERROR_UNKNOWN;
        }
    }

    ONECLOUD_API OneCloud_Error onecloud_storage_write_file(OneCloud_StorageHandle* handle, const char* virtual_path, const uint8_t* data, size_t size) {
        if (!handle || !virtual_path || (!data && size > 0)) return ONECLOUD_ERROR_UNKNOWN;
        auto storage = reinterpret_cast<onecloud::CloudStorage*>(handle);
        try {
            std::span<const std::byte> data_span(reinterpret_cast<const std::byte*>(data), size);
            auto result = storage->writeFile(virtual_path, data_span);
            if (result) {
                return ONECLOUD_SUCCESS;
            }
            return to_c_error(result.error());
        }
        catch (...) {
            return ONECLOUD_ERROR_UNKNOWN;
        }
    }

    ONECLOUD_API OneCloud_Error onecloud_storage_delete_file(OneCloud_StorageHandle* handle, const char* virtual_path) {
        if (!handle || !virtual_path) return ONECLOUD_ERROR_UNKNOWN;
        auto storage = reinterpret_cast<onecloud::CloudStorage*>(handle);
        try {
            auto result = storage->deleteFile(virtual_path);
            if (result) {
                return ONECLOUD_SUCCESS;
            }
            return to_c_error(result.error());
        }
        catch (...) {
            return ONECLOUD_ERROR_UNKNOWN;
        }
    }

    ONECLOUD_API OneCloud_Error onecloud_storage_list_files(OneCloud_StorageHandle* handle, char*** out_file_list, size_t* out_count) {
        if (!handle || !out_file_list || !out_count) return ONECLOUD_ERROR_UNKNOWN;
        auto storage = reinterpret_cast<onecloud::CloudStorage*>(handle);
        try {
            auto result = storage->listFiles();
            if (result) {
                auto& files = *result;
                *out_count = files.size();
                if (files.empty()) {
                    *out_file_list = nullptr;
                    return ONECLOUD_SUCCESS;
                }

                *out_file_list = static_cast<char**>(std::malloc(files.size() * sizeof(char*)));
                if (!*out_file_list) return ONECLOUD_ERROR_OUT_OF_MEMORY;

                for (size_t i = 0; i < files.size(); ++i) {
#ifdef _WIN32
                    (*out_file_list)[i] = _strdup(files[i].c_str());
#else
                    (*out_file_list)[i] = strdup(files[i].c_str());
#endif
                    if (!(*out_file_list)[i]) {
                        for (size_t j = 0; j < i; ++j) {
                            std::free((*out_file_list)[j]);
                        }
                        std::free(*out_file_list);
                        return ONECLOUD_ERROR_OUT_OF_MEMORY;
                    }
                }
                return ONECLOUD_SUCCESS;
            }
            return to_c_error(result.error());
        }
        catch (...) {
            return ONECLOUD_ERROR_UNKNOWN;
        }
    }

    // --- Memory Management for C API Allocations ---

    ONECLOUD_API void onecloud_free_file_list(char** file_list, size_t count) {
        if (!file_list) return;
        for (size_t i = 0; i < count; ++i) {
            std::free(file_list[i]);
        }
        std::free(file_list);
    }

    ONECLOUD_API void onecloud_free_data_buffer(uint8_t* data) {
        std::free(data);
    }

} // extern "C"
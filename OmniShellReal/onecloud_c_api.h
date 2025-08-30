Copyright © 2025 Cadell Richard Anderson

#ifndef ONECLOUD_C_API_H
#define ONECLOUD_C_API_H

#include <stddef.h>
#include <stdint.h>

// Define import/export macros for Windows DLLs
#ifdef _WIN32
#ifdef ONECLOUD_EXPORTS
#define ONECLOUD_API __declspec(dllexport)
#else
#define ONECLOUD_API __declspec(dllimport)
#endif
#else
#define ONECLOUD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

    // Opaque handle to the internal C++ CloudStorage object.
    // The user of this C API never needs to know the internal structure.
    struct OneCloud_StorageHandle;
    typedef struct OneCloud_StorageHandle OneCloud_StorageHandle;

    // Public error codes for the C API, mirroring the C++ enum.
    typedef enum {
        ONECLOUD_SUCCESS = 0,
        ONECLOUD_ERROR_CONTAINER_NOT_FOUND,
        ONECLOUD_ERROR_INVALID_PASSWORD,
        ONECLOUD_ERROR_INVALID_CONTAINER_FORMAT,
        ONECLOUD_ERROR_ACCESS_DENIED,
        ONECLOUD_ERROR_FILE_EXISTS,
        ONECLOUD_ERROR_FILE_NOT_FOUND,
        ONECLOUD_ERROR_IO_ERROR,
        ONECLOUD_ERROR_OUT_OF_MEMORY,
        ONECLOUD_ERROR_ENCRYPTION_FAILED, // <-- FIXED: Added missing error code
        ONECLOUD_ERROR_UNKNOWN
    } OneCloud_Error;

    // --- Container Operations ---

    /**
     * @brief Creates a new, empty.ocv container.
     * @param path The filesystem path where the container will be created.
     * @param password The password to encrypt the container with.
     * @param handle A pointer to a handle that will be populated on success.
     * @return ONECLOUD_SUCCESS on success, or an error code on failure.
     */
    ONECLOUD_API OneCloud_Error onecloud_storage_create(const char* path, const char* password, OneCloud_StorageHandle** handle);

    /**
     * @brief Opens an existing.ocv container.
     * @param path The filesystem path of the container to open.
     * @param password The password for the container.
     * @param handle A pointer to a handle that will be populated on success.
     * @return ONECLOUD_SUCCESS on success, or an error code on failure.
     */
    ONECLOUD_API OneCloud_Error onecloud_storage_open(const char* path, const char* password, OneCloud_StorageHandle** handle);

    /**
     * @brief Closes a container handle and releases all associated resources.
     * @param handle The handle to close.
     */
    ONECLOUD_API void onecloud_storage_close(OneCloud_StorageHandle* handle);


    // --- File Operations ---

    /**
     * @brief Reads the full content of a virtual file from the container.
     * @param handle A valid container handle.
     * @param virtual_path The path of the file inside the container.
     * @param out_data A pointer that will receive the allocated buffer with the file data. The caller must free this buffer using onecloud_free_data_buffer.
     * @param out_size A pointer that will receive the size of the data buffer.
     * @return ONECLOUD_SUCCESS on success, or an error code on failure.
     */
    ONECLOUD_API OneCloud_Error onecloud_storage_read_file(OneCloud_StorageHandle* handle, const char* virtual_path, uint8_t** out_data, size_t* out_size);

    /**
     * @brief Writes data to a virtual file in the container, creating it if it doesn't exist or overwriting it if it does.
     * @param handle A valid container handle.
     * @param virtual_path The path of the file inside the container.
     * @param data A pointer to the data to write.
     * @param size The size of the data to write.
     * @return ONECLOUD_SUCCESS on success, or an error code on failure.
     */
    ONECLOUD_API OneCloud_Error onecloud_storage_write_file(OneCloud_StorageHandle* handle, const char* virtual_path, const uint8_t* data, size_t size);

    /**
     * @brief Deletes a virtual file from the container.
     * @param handle A valid container handle.
     * @param virtual_path The path of the file to delete inside the container.
     * @return ONECLOUD_SUCCESS on success, or an error code on failure.
     */
    ONECLOUD_API OneCloud_Error onecloud_storage_delete_file(OneCloud_StorageHandle* handle, const char* virtual_path);

    /**
     * @brief Lists all files within the container.
     * @param handle A valid container handle.
     * @param out_file_list A pointer that will receive the allocated array of C-strings. The caller must free this list using onecloud_free_file_list.
     * @param out_count A pointer that will receive the number of files in the list.
     * @return ONECLOUD_SUCCESS on success, or an error code on failure.
     */
    ONECLOUD_API OneCloud_Error onecloud_storage_list_files(OneCloud_StorageHandle* handle, char*** out_file_list, size_t* out_count);


    // --- Memory Management for C API Allocations ---

    /**
     * @brief Frees a list of file paths allocated by onecloud_storage_list_files.
     * @param file_list The list to free.
     * @param count The number of elements in the list.
     */
    ONECLOUD_API void onecloud_free_file_list(char** file_list, size_t count);

    /**
     * @brief Frees a data buffer allocated by onecloud_storage_read_file.
     * @param data The buffer to free.
     */
    ONECLOUD_API void onecloud_free_data_buffer(uint8_t* data);

#ifdef __cplusplus
}
#endif

#endif // ONECLOUD_C_API_H

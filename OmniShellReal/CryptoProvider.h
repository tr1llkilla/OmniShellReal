//CryptoProvider.h

#pragma once

#include <vector>
#include <string>
#include <expected>
#include <span>
#include "CloudAPI.h" // For the CloudError enum

namespace onecloud {

    /**
     * @class CryptoProvider
     * @brief Provides a static, C++-native interface for all cryptographic operations.
     *
     * This class abstracts the underlying crypto library (Botan), offering a stable
     * and easy-to-use set of functions for key derivation, authenticated encryption,
     * and random data generation.
     */
    class CryptoProvider {
    public:
        /**
         * @brief Derives a strong cryptographic key from a user-provided password and salt.
         * @param password The user's password.
         * @param salt A unique, per-container salt.
         * @return A 32-byte key on success, or a CloudError on failure.
         */
        static std::expected<std::vector<std::byte>, CloudError> derive_key_from_password(
            const std::string& password,
            std::span<const std::byte> salt);

        /**
         * @brief Encrypts and authenticates a block of data using ChaCha20-Poly1305.
         * @param plaintext The data to encrypt.
         * @param key The 32-byte encryption key.
         * @return A single buffer containing [nonce][ciphertext][authentication_tag], or a CloudError.
         */
        static std::expected<std::vector<std::byte>, CloudError> encrypt(
            std::span<const std::byte> plaintext,
            std::span<const std::byte> key);

        /**
         * @brief Decrypts and verifies a block of data encrypted with ChaCha20-Poly1305.
         * @param encrypted_data The buffer containing [nonce][ciphertext][authentication_tag].
         * @param key The 32-byte decryption key.
         * @return The original plaintext on success, or a CloudError if decryption or verification fails.
         */
        static std::expected<std::vector<std::byte>, CloudError> decrypt(
            std::span<const std::byte> encrypted_data,
            std::span<const std::byte> key);

        /**
         * @brief Generates a buffer of cryptographically secure random bytes.
         * @param size The number of random bytes to generate.
         * @return A vector containing the random bytes.
         */
        static std::vector<std::byte> random_bytes(size_t size);

        // --- Cryptographic Constants ---
        // These define the required sizes for keys, salts, etc., for the container format.
        static constexpr size_t key_length() { return 32; } // 256-bit key
        static constexpr size_t salt_length() { return 16; } // 128-bit salt
        static constexpr size_t nonce_length() { return 12; } // 96-bit nonce for ChaCha20
        static constexpr size_t tag_length() { return 16; } // 128-bit Poly1305 tag
    };

} // namespace onecloud
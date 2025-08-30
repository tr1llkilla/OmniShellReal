#include "CryptoProvider.h"

// --- Botan Dependency Headers ---
#include <botan/auto_rng.h>
#include <botan/aead.h>
#include <botan/exceptn.h>
#include <botan/pwdhash.h> // <-- REQUIRED for Botan 3 password hashing

namespace onecloud {

    std::vector<std::byte> CryptoProvider::random_bytes(size_t size) {
        Botan::AutoSeeded_RNG rng;
        std::vector<std::byte> buffer(size);
        rng.randomize(reinterpret_cast<uint8_t*>(buffer.data()), size);
        return buffer;
    }

    std::expected<std::vector<std::byte>, CloudError> CryptoProvider::derive_key_from_password(
        const std::string& password,
        std::span<const std::byte> salt)
    {
        if (salt.size() < 8) {
            return std::unexpected(onecloud::CloudError::EncryptionFailed);
        }
        std::vector<std::byte> derived_key(key_length());

        const size_t m_cost = 65536; // Memory cost
        const size_t t_cost = 2;     // Iterations
        const size_t p_factor = 1; // Parallelism

        try {
            // Correct code for Botan 3.x API
            if (auto pwdhash_family = Botan::PasswordHashFamily::create("Argon2id")) {
                auto pwdhash = pwdhash_family->from_params(t_cost, m_cost, p_factor);
                pwdhash->derive_key(
                    reinterpret_cast<uint8_t*>(derived_key.data()), derived_key.size(),
                    password.c_str(), password.size(),
                    reinterpret_cast<const uint8_t*>(salt.data()), salt.size()
                );
            }
            else {
                return std::unexpected(onecloud::CloudError::EncryptionFailed);
            }
        }
        catch (const std::exception&) {
            return std::unexpected(onecloud::CloudError::EncryptionFailed);
        }

        return derived_key;
    }

    std::expected<std::vector<std::byte>, CloudError> CryptoProvider::encrypt(
        std::span<const std::byte> plaintext,
        std::span<const std::byte> key)
    {
        if (key.size() != key_length()) {
            return std::unexpected(onecloud::CloudError::EncryptionFailed);
        }

        try {
            auto aead = Botan::AEAD_Mode::create_or_throw("ChaCha20Poly1305", Botan::Cipher_Dir::Encryption);
            aead->set_key(reinterpret_cast<const uint8_t*>(key.data()), key.size());

            auto nonce = random_bytes(nonce_length());
            aead->start(reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size());

            // FIXED: Explicitly cast from std::byte* to uint8_t* for the constructor
            Botan::secure_vector<uint8_t> buffer(
                reinterpret_cast<const uint8_t*>(plaintext.data()),
                reinterpret_cast<const uint8_t*>(plaintext.data()) + plaintext.size()
            );
            aead->finish(buffer);

            std::vector<std::byte> result;
            result.reserve(nonce.size() + buffer.size());
            result.insert(result.end(), nonce.begin(), nonce.end());
            result.insert(result.end(), reinterpret_cast<const std::byte*>(buffer.data()), reinterpret_cast<const std::byte*>(buffer.data()) + buffer.size());

            return result;
        }
        catch (const std::exception&) {
            return std::unexpected(onecloud::CloudError::EncryptionFailed);
        }
    }

    std::expected<std::vector<std::byte>, CloudError> CryptoProvider::decrypt(
        std::span<const std::byte> encrypted_data,
        std::span<const std::byte> key)
    {
        if (key.size() != key_length() || encrypted_data.size() < nonce_length() + tag_length()) {
            return std::unexpected(onecloud::CloudError::EncryptionFailed);
        }

        try {
            auto aead = Botan::AEAD_Mode::create_or_throw("ChaCha20Poly1305", Botan::Cipher_Dir::Decryption);
            aead->set_key(reinterpret_cast<const uint8_t*>(key.data()), key.size());

            auto nonce = encrypted_data.subspan(0, nonce_length());
            aead->start(reinterpret_cast<const uint8_t*>(nonce.data()), nonce.size());

            auto ciphertext_and_tag = encrypted_data.subspan(nonce_length());
            // FIXED: Explicitly cast from std::byte* to uint8_t* for the constructor
            Botan::secure_vector<uint8_t> buffer(
                reinterpret_cast<const uint8_t*>(ciphertext_and_tag.data()),
                reinterpret_cast<const uint8_t*>(ciphertext_and_tag.data()) + ciphertext_and_tag.size()
            );
            aead->finish(buffer);

            return std::vector<std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), reinterpret_cast<const std::byte*>(buffer.data()) + buffer.size());
        }
        catch (const Botan::Invalid_Authentication_Tag&) {
            return std::unexpected(onecloud::CloudError::InvalidPassword);
        }
        catch (const std::exception&) {
            return std::unexpected(onecloud::CloudError::EncryptionFailed);
        }
    }

} // namespace onecloud
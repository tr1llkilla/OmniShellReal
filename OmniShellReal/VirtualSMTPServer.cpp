// =================================================================
// VirtualSMTPServer.cpp
// Implements the in-process virtualized SMTP relay.
// Decrypts ChaCha20 → XOR to get plaintext RFC-822, then does an
// in-memory AES-256-GCM wrap/unwrap (for layered protection inside
// the relay boundary), and finally transmits the readable message
// via TLS to the real SMTP server (so recipients see plaintext).
// =================================================================
#include "VirtualSMTPServer.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <iostream>
#include <sstream>
#include <climits>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

    // --- added helper: safe SOCKET->int conversion for APIs expecting int fds ---
    static inline int socket_to_fd(SOCKET s) {
        if (s == INVALID_SOCKET) return -1;
        if (s > static_cast<SOCKET>(INT_MAX)) {
            std::cerr << "socket_to_fd: SOCKET too large for int fd.\n";
            return -1;
        }
        return static_cast<int>(s);
    }

    // --- helpers shared with ScriptRunner but redefined to keep this TU standalone ---

    std::string base64_encode(const std::string& in) {
        static const char* enc_table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, valb = -6;
        for (uint8_t c : in) {
            val = (val << 8) + c;
            valb += 8;
            while (valb >= 0) {
                out.push_back(enc_table[(val >> valb) & 0x3F]);
                valb -= 6;
            }
        }
        if (valb > -6) out.push_back(enc_table[((val << 8) >> (valb + 8)) & 0x3F]);
        while (out.size() % 4) out.push_back('=');
        return out;
    }

    inline std::vector<unsigned char>
        rotating_xor(const std::vector<unsigned char>& data, const std::vector<unsigned char>& key) {
        if (key.empty()) return data;
        std::vector<unsigned char> out(data.size());
        const size_t k = key.size();
        for (size_t i = 0; i < data.size(); ++i) {
            out[i] = static_cast<unsigned char>(data[i] ^ key[i % k]);
        }
        return out;
    }

    inline std::vector<unsigned char>
        chacha20_crypt(const std::vector<unsigned char>& input,
            const std::vector<unsigned char>& key,   // 32 bytes
            const std::vector<unsigned char>& nonce) // 12 bytes
    {
        std::vector<unsigned char> out(input.size());
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return {};

        if (EVP_EncryptInit_ex(ctx, EVP_chacha20(), NULL, key.data(), nonce.data()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        int len = 0;
        if (EVP_EncryptUpdate(ctx, out.data(), &len, input.data(), static_cast<int>(input.size())) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return {};
        }

        EVP_CIPHER_CTX_free(ctx);
        return out;
    }

    // AES-256-GCM (encrypt then decrypt immediately inside the relay layer)
    inline bool aes256gcm_encrypt(const std::vector<unsigned char>& plaintext,
        const std::vector<unsigned char>& key,
        const std::vector<unsigned char>& iv,
        std::vector<unsigned char>& ciphertext,
        std::vector<unsigned char>& tag)
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;

        bool ok = false;
        do {
            if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) break;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)iv.size(), NULL) != 1) break;
            if (EVP_EncryptInit_ex(ctx, NULL, NULL, key.data(), iv.data()) != 1) break;

            ciphertext.resize(plaintext.size());
            int len = 0, total = 0;
            if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), (int)plaintext.size()) != 1) break;
            total += len;

            if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + total, &len) != 1) break;
            total += len;
            ciphertext.resize(total);

            tag.resize(16);
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data()) != 1) break;

            ok = true;
        } while (false);

        EVP_CIPHER_CTX_free(ctx);
        return ok;
    }

    inline bool aes256gcm_decrypt(const std::vector<unsigned char>& ciphertext,
        const std::vector<unsigned char>& key,
        const std::vector<unsigned char>& iv,
        const std::vector<unsigned char>& tag,
        std::vector<unsigned char>& plaintext)
    {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;

        bool ok = false;
        do {
            if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) break;
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, (int)iv.size(), NULL) != 1) break;
            if (EVP_DecryptInit_ex(ctx, NULL, NULL, key.data(), iv.data()) != 1) break;

            plaintext.resize(ciphertext.size());
            int len = 0, total = 0;
            if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), (int)ciphertext.size()) != 1) break;
            total += len;

            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, (int)tag.size(), const_cast<unsigned char*>(tag.data())) != 1) break;

            if (EVP_DecryptFinal_ex(ctx, plaintext.data() + total, &len) != 1) break;
            total += len;
            plaintext.resize(total);
            ok = true;
        } while (false);

        EVP_CIPHER_CTX_free(ctx);
        return ok;
    }

    void wait_and_read(SSL* ssl) {
        char buffer[4096];
        int bytesReceived = SSL_read(ssl, buffer, sizeof(buffer) - 1);
        if (bytesReceived > 0) {
            buffer[bytesReceived] = '\0';
            std::cout << "[SERVER]: " << buffer;
        }
    }

    void send_command(SSL* ssl, const std::string& cmd) {
        std::cout << "[CLIENT]: " << cmd << std::endl;
        std::string msg = cmd + "\r\n";
        SSL_write(ssl, msg.c_str(), (int)msg.length());
        wait_and_read(ssl);
    }

    // --- added helper: safe suffix check ---
    static inline bool ends_with(const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
            std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
    }

    bool smtp_send_readable_tls(const std::string& smtp_server, const std::string& port,
        const std::string& sender, const std::string& username, const std::string& password,
        const std::vector<std::string>& recipients,
        const std::string& subject,
        const std::string& readableEmailRFC822)
    {
        // Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed.\n";
            return false;
        }

        struct addrinfo hints = {}, * res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(smtp_server.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
            std::cerr << "Failed to resolve SMTP host.\n";
            WSACleanup();
            return false;
        }

        SOCKET sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (connect(sock, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
            std::cerr << "Failed to connect to SMTP server.\n";
            freeaddrinfo(res);
            closesocket(sock);
            WSACleanup();
            return false;
        }
        freeaddrinfo(res);

        // OpenSSL init
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();

        const SSL_METHOD* method = TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);
        SSL* ssl = SSL_new(ctx);

        // --- addition: enable SNI for proper certificate selection ---
        SSL_set_tlsext_host_name(ssl, smtp_server.c_str());

        // --- modified to use safe int fd for BIO_new_socket ---
        int fd = socket_to_fd(sock);
        BIO* bio = BIO_new_socket(fd, BIO_NOCLOSE);
        SSL_set_bio(ssl, bio, bio);

        // Initial cleartext part to negotiate STARTTLS
        {
            char buffer[4096];
            int n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                std::cout << "[SERVER]: " << buffer;
            }

            std::string ehlo = "EHLO localhost\r\n";
            send(sock, ehlo.c_str(), (int)ehlo.length(), 0);
            n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                std::cout << "[SERVER]: " << buffer;
            }

            std::string starttls = "STARTTLS\r\n";
            send(sock, starttls.c_str(), (int)starttls.length(), 0);
            n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                std::cout << "[SERVER]: " << buffer;
            }
        }

        // Upgrade to TLS
        if (SSL_connect(ssl) <= 0) {
            std::cerr << "SSL handshake failed.\n";
            ERR_print_errors_fp(stderr);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            closesocket(sock);
            WSACleanup();
            return false;
        }

        // Auth & send
        send_command(ssl, "EHLO localhost");
        send_command(ssl, "AUTH LOGIN");
        send_command(ssl, base64_encode(username));
        send_command(ssl, base64_encode(password));
        send_command(ssl, "MAIL FROM:<" + sender + ">");
        for (const auto& r : recipients)
            send_command(ssl, "RCPT TO:<" + r + ">");
        send_command(ssl, "DATA");

        // The readableEmailRFC822 must include trailing "\r\n.\r\n".
        // --- addition: append terminator if missing (additive, no removals) ---
        SSL_write(ssl, readableEmailRFC822.c_str(), (int)readableEmailRFC822.size());
        if (!ends_with(readableEmailRFC822, "\r\n.\r\n")) {
            static const char dot_term[] = "\r\n.\r\n";
            SSL_write(ssl, dot_term, (int)sizeof(dot_term) - 1);
        }
        wait_and_read(ssl);
        send_command(ssl, "QUIT");

        // Cleanup
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        closesocket(sock);
        WSACleanup();
        return true;
    }

} // namespace

bool VirtualSMTPServer::RelayAndSend(const std::string& smtp_server, const std::string& port,
    const std::string& sender, const std::string& username, const std::string& password,
    const std::vector<std::string>& recipients,
    const std::string& subject,
    const std::vector<unsigned char>& stage2Message,
    const CryptoBundle& crypto)
{
    // -----------------------------------------
    // 1) Decrypt ChaCha20 then reverse Rotating XOR to get plaintext
    // -----------------------------------------
    // ChaCha decryption = encryption for stream cipher; apply same function
    std::vector<unsigned char> afterChaCha = chacha20_crypt(stage2Message, crypto.chachaKey, crypto.chachaNonce);
    if (afterChaCha.empty()) {
        std::cerr << "Relay: ChaCha20 decrypt failed.\n";
        return false;
    }
    std::vector<unsigned char> plaintextBytes = rotating_xor(afterChaCha, crypto.xorKey);
    std::string readableRFC822(plaintextBytes.begin(), plaintextBytes.end());

    // -----------------------------------------
    // 2) Optional in-memory AES-256-GCM wrap/unwrap
    //    (kept inside relay boundary for layered protection)
    // -----------------------------------------
    std::vector<unsigned char> aesCipher, gcmTag, aesPlain;
    if (!aes256gcm_encrypt(plaintextBytes, crypto.aesKey, crypto.aesIv, aesCipher, gcmTag)) {
        std::cerr << "Relay: AES-GCM encryption failed.\n";
        return false;
    }
    if (!aes256gcm_decrypt(aesCipher, crypto.aesKey, crypto.aesIv, gcmTag, aesPlain)) {
        std::cerr << "Relay: AES-GCM decryption failed.\n";
        return false;
    }
    // Sanity: make sure we recovered the original
    if (aesPlain != plaintextBytes) {
        std::cerr << "Relay: AES-GCM roundtrip mismatch.\n";
        return false;
    }

    // -----------------------------------------
    // 3) Deliver readable message to real SMTP over TLS
    // -----------------------------------------
    // NOTE: readableRFC822 is expected to already include "\r\n.\r\n"
    return smtp_send_readable_tls(smtp_server, port, sender, username, password, recipients, subject, readableRFC822);
}

#endif // _WIN32
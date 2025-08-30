// source_network_pcap.h
#pragma once
#include "types.h"
#include "packet_frame.h"
#include <pcap.h>
#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstring>
#include <chrono>
#include <vector>

namespace ironrouter {

    // ✅ ADDED: Definition for the missing struct
    struct LiveCaptureDevice {
        int id;
        std::string name;
        std::string description;
    };

    class SourceNetworkPcap {
    public:
        SourceNetworkPcap() {}
        ~SourceNetworkPcap() {
            stop();
        }

        using FrameSink = std::function<void(const u8*, size_t, const PcapRecordHeader&)>;

        void set_frame_sink(FrameSink sink) {
            sink_ = std::move(sink);
        }

        // Static function to list available devices
        static std::vector<LiveCaptureDevice> list_devices() {
            std::vector<LiveCaptureDevice> devices;
            char errbuf[PCAP_ERRBUF_SIZE]{};
            pcap_if_t* alldevs;
            if (pcap_findalldevs(&alldevs, errbuf) == -1) {
                std::cerr << "[ironrouter] Error finding devices: " << errbuf << "\n";
                return devices;
            }

            int i = 0;
            for (pcap_if_t* d = alldevs; d; d = d->next) {
                LiveCaptureDevice dev;
                dev.id = i++;
                dev.name = d->name;
                if (d->description) {
                    dev.description = d->description;
                }
                devices.push_back(dev);
            }
            pcap_freealldevs(alldevs);
            return devices;
        }


        bool start_listen(int deviceID, u16 port, const std::string& capture_file, bool promisc) {
            if (handle_) {
                std::cerr << "[ironrouter] Already have an active handle. Stop first.\n";
                return false;
            }

            char errbuf[PCAP_ERRBUF_SIZE]{};
            pcap_if_t* alldevs;
            if (pcap_findalldevs(&alldevs, errbuf) == -1) {
                std::cerr << "[ironrouter] Error finding devices: " << errbuf << "\n";
                return false;
            }
            pcap_if_t* dev = alldevs;
            for (int i = 0; dev && i < deviceID; i++) {
                dev = dev->next;
            }
            if (!dev) {
                std::cerr << "[ironrouter] Invalid device ID: " << deviceID << "\n";
                pcap_freealldevs(alldevs);
                return false;
            }

            // Note: capture_file is unused in this implementation but kept for signature compatibility
            (void)capture_file;

            handle_ = pcap_open_live(dev->name, 65536, promisc ? 1 : 0, 1, errbuf);
            pcap_freealldevs(alldevs);

            if (!handle_) {
                std::cerr << "[ironrouter] Error opening device for listen: " << errbuf << "\n";
                return false;
            }

            // Apply port filter
            if (port > 0) {
                struct bpf_program fp;
                std::string filter_exp = "udp port " + std::to_string(port);
                if (pcap_compile(handle_, &fp, filter_exp.c_str(), 0, PCAP_NETMASK_UNKNOWN) == -1) {
                    std::cerr << "[ironrouter] Couldn't parse filter " << filter_exp << ": " << pcap_geterr(handle_) << "\n";
                    pcap_close(handle_);
                    handle_ = nullptr;
                    return false;
                }
                if (pcap_setfilter(handle_, &fp) == -1) {
                    std::cerr << "[ironrouter] Couldn't install filter " << filter_exp << ": " << pcap_geterr(handle_) << "\n";
                    pcap_freecode(&fp);
                    pcap_close(handle_);
                    handle_ = nullptr;
                    return false;
                }
                pcap_freecode(&fp);
            }

            listen_thread_ = std::thread(&SourceNetworkPcap::run_capture_loop, this);
            std::cout << "[ironrouter] Listener started on device " << deviceID << ".\n";
            return true;
        }

        void stop() {
            listen_running_ = false;
            if (handle_) {
                pcap_breakloop(handle_);
            }
            if (listen_thread_.joinable()) {
                listen_thread_.join();
            }
            stop_uplink_worker();
            if (handle_) {
                pcap_close(handle_);
                handle_ = nullptr;
            }
        }

        bool open_device_for_send(int deviceID) {
            char errbuf[PCAP_ERRBUF_SIZE]{};
            pcap_if_t* alldevs;
            if (pcap_findalldevs(&alldevs, errbuf) == -1) {
                std::cerr << "[ironrouter] Error finding devices: " << errbuf << "\n";
                return false;
            }
            pcap_if_t* dev = alldevs;
            for (int i = 0; dev && i < deviceID; i++) {
                dev = dev->next;
            }
            if (!dev) {
                std::cerr << "[ironrouter] Invalid device ID: " << deviceID << "\n";
                pcap_freealldevs(alldevs);
                return false;
            }
            handle_ = pcap_open_live(dev->name, 65536, 1, 1000, errbuf);
            pcap_freealldevs(alldevs);
            if (!handle_) {
                std::cerr << "[ironrouter] Error opening device for send: " << errbuf << "\n";
                return false;
            }
            return true;
        }

        bool send_packet(const PacketFrame& frame) {
            if (!handle_) return false;
            if (pcap_sendpacket(handle_, frame.data.data(), static_cast<int>(frame.data.size())) != 0) {
                std::cerr << "[ironrouter] Error sending packet: " << pcap_geterr(handle_) << "\n";
                return false;
            }
            return true;
        }

        void start_uplink_worker() {
            if (!handle_) return;
            uplink_running_ = true;
            uplink_thread_ = std::thread([this]() {
                while (uplink_running_) {
                    PacketFrame frame;
                    InProcessPacketReader reader(g_uplink_buf);
                    if (reader.read(frame)) { // This now blocks until a packet is available
                        if (!send_packet(frame)) {
                            std::cerr << "[ironrouter] uplink_worker: send failed\n";
                        }
                    }
                }
                });
        }

        void stop_uplink_worker() {
            uplink_running_ = false;
            if (uplink_thread_.joinable()) {
                uplink_thread_.join();
            }
        }

    private:
        static void pcap_packet_handler(u_char* user, const struct pcap_pkthdr* header, const u_char* bytes) {
            SourceNetworkPcap* self = reinterpret_cast<SourceNetworkPcap*>(user);
            if (self && self->sink_) {
                PcapRecordHeader rhdr;
                rhdr.ts_sec = header->ts.tv_sec;
                rhdr.ts_usec = header->ts.tv_usec;
                rhdr.incl_len = header->caplen;
                rhdr.orig_len = header->len;
                self->sink_(bytes, header->caplen, rhdr);
            }
        }

        void run_capture_loop() {
            listen_running_ = true;
            pcap_loop(handle_, -1, pcap_packet_handler, reinterpret_cast<u_char*>(this));
            listen_running_ = false;
        }

        pcap_t* handle_{ nullptr };
        FrameSink sink_;

        std::thread listen_thread_;
        std::atomic<bool> listen_running_{ false };

        std::thread uplink_thread_;
        std::atomic<bool> uplink_running_{ false };
    };

} // namespace ironrouter

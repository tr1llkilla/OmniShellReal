// live_capture.cpp

#include "live_capture.h"
#include <pcap.h>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstring> // For memset

namespace ironrouter {

    // The implementation details are hidden within this struct.
    struct LiveCapture::Impl {
        pcap_t* pcap_handle = nullptr;
        std::thread capture_thread;
        std::atomic<bool> is_capturing{ false };
        FrameCallback frame_callback;

        // This is the function that runs on the background thread.
        void capture_loop() {
            // pcap_loop will block until pcap_breakloop is called or an error occurs.
            pcap_loop(pcap_handle, -1, packet_handler, reinterpret_cast<u_char*>(this));
            is_capturing = false;
        }

        // This is the C-style callback that pcap_loop will invoke for each packet.
        static void packet_handler(u_char* user, const struct pcap_pkthdr* header, const u_char* bytes) {
            LiveCapture::Impl* self = reinterpret_cast<LiveCapture::Impl*>(user);
            if (self && self->frame_callback) {
                // Debug log
                std::cerr << "[LiveCapture] Packet captured: len="
                    << header->len
                    << " caplen=" << header->caplen
                    << " ts=" << header->ts.tv_sec
                    << "." << header->ts.tv_usec
                    << "\n";

                PcapRecordHeader rhdr;
                rhdr.ts_sec = header->ts.tv_sec;
                rhdr.ts_usec = header->ts.tv_usec;
                rhdr.incl_len = header->caplen;
                rhdr.orig_len = header->len;
                self->frame_callback(bytes, header->caplen, rhdr);
            }
        }
    };

    LiveCapture::LiveCapture() : pimpl(std::make_unique<Impl>()) {}

    LiveCapture::~LiveCapture() {
        stop_capture();
    }

    std::vector<NetworkDevice> LiveCapture::list_devices() {
        std::vector<NetworkDevice> devices;
        pcap_if_t* alldevs;
        char errbuf[PCAP_ERRBUF_SIZE];

        if (pcap_findalldevs(&alldevs, errbuf) == -1) {
            std::cerr << "[LiveCapture] Error finding devices: " << errbuf << std::endl;
            return devices;
        }

        int i = 0;
        for (pcap_if_t* d = alldevs; d; d = d->next) {
            NetworkDevice dev;
            dev.id = i++;
            dev.name = d->name;
            if (d->description) {
                dev.description = d->description;
            }
            else {
                dev.description = "No description available";
            }
            devices.push_back(dev);
        }

        pcap_freealldevs(alldevs);
        return devices;
    }

    bool LiveCapture::start_capture(
        int device_index,
        FrameCallback callback,
        const std::string& filter_expression
    ) {
        if (pimpl->is_capturing) {
            return false; // Already capturing
        }

        pcap_if_t* alldevs;
        char errbuf[PCAP_ERRBUF_SIZE];
        if (pcap_findalldevs(&alldevs, errbuf) == -1) {
            std::cerr << "[LiveCapture] Error finding devices: " << errbuf << std::endl;
            return false;
        }

        pcap_if_t* d = alldevs;
        for (int i = 0; i < device_index && d; i++) {
            d = d->next;
        }

        if (!d) {
            std::cerr << "[LiveCapture] Error: Device index out of bounds." << std::endl;
            pcap_freealldevs(alldevs);
            return false;
        }

        pimpl->pcap_handle = pcap_open_live(d->name, 65536, 1, 1000, errbuf);
        pcap_freealldevs(alldevs);

        if (pimpl->pcap_handle == nullptr) {
            std::cerr << "[LiveCapture] Unable to open the adapter. "
                << d->name << " is not supported by Npcap" << std::endl;
            return false;
        }

        std::string actual_filter = filter_expression;
        if (actual_filter.empty()) {
            actual_filter = "ip or ip6";
            std::cerr << "[LiveCapture] No filter specified — using default noisy filter: "
                << actual_filter << "\n";
        }

        if (!actual_filter.empty()) {
            struct bpf_program fcode;
            if (pcap_compile(pimpl->pcap_handle, &fcode, actual_filter.c_str(), 1, PCAP_NETMASK_UNKNOWN) < 0) {
                std::cerr << "[LiveCapture] Warning: pcap_compile failed for filter '"
                    << actual_filter << "': " << pcap_geterr(pimpl->pcap_handle) << "\n";
            }
            else {
                if (pcap_setfilter(pimpl->pcap_handle, &fcode) < 0) {
                    std::cerr << "[LiveCapture] Warning: pcap_setfilter failed for '"
                        << actual_filter << "': " << pcap_geterr(pimpl->pcap_handle) << "\n";
                }
                else {
                    std::cerr << "[LiveCapture] BPF filter set: " << actual_filter << "\n";
                }
                pcap_freecode(&fcode);
            }
        }

        std::cerr << "[LiveCapture] Starting capture on device index " << device_index << "\n";

        pimpl->frame_callback = callback;
        pimpl->is_capturing = true;
        pimpl->capture_thread = std::thread(&LiveCapture::Impl::capture_loop, pimpl.get());

        return true;
    }

    void LiveCapture::stop_capture() {
        if (pimpl->is_capturing && pimpl->pcap_handle) {
            pimpl->is_capturing = false;
            pcap_breakloop(pimpl->pcap_handle);
            if (pimpl->capture_thread.joinable()) {
                pimpl->capture_thread.join();
            }
            pcap_close(pimpl->pcap_handle);
            pimpl->pcap_handle = nullptr;
        }
    }

} // namespace ironrouter

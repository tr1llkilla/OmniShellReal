Copyright © 2025 Cadell Richard Anderson

//===============================
// PMU.cpp
//===================================
#include "PMU.h"
#include "OmniAIManager.h" 

#include <vector>
#include <string>
#include <sstream>    
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <chrono>
#include <utility>

#include <functional>
#include <map>
#include <thread>
#include <atomic>
#include <cstdint>
#include <tuple>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <processthreadsapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "Advapi32.lib")
#ifndef THREAD_QUERY_LIMITED_INFORMATION
#define THREAD_QUERY_LIMITED_INFORMATION 0x0800
#endif
#elif __linux__
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <sys/resource.h>
#endif

namespace PMU {
    std::atomic_bool g_pmuStopFlag{ false };
    static std::string g_lastSummary;
    static std::mutex g_summaryMutex;


    std::string getRecentPmuSummary() {
        std::lock_guard<std::mutex> lock(g_summaryMutex);
        return g_lastSummary;
    }

    void MonitorSelf(const std::chrono::milliseconds interval,
        const size_t topN,
        std::function<void(const std::string&)> onSummary,
        std::atomic_bool* stopFlag)
    {
        ProcessSample prev = SampleSelf();
        std::this_thread::sleep_for(interval);
        while (true) {
            if (stopFlag && stopFlag->load(std::memory_order_relaxed)) break;

            ProcessSample curr = SampleSelf();
            std::string summary = BuildTopThreadSummary(prev, curr, topN);

            {
                std::lock_guard<std::mutex> lock(g_summaryMutex);
                g_lastSummary = summary;
            }
            // Always push to OmniAIManager for system‑wide availability
            OmniAIManager::setRecentPmuSummary(summary);

            if (onSummary) onSummary(summary);
            else std::cout << summary << std::endl;

            prev = std::move(curr);
            std::this_thread::sleep_for(interval);
        }
    }

#ifdef _WIN32
    static inline double fileTimeToMs(const FILETIME& ft) {
        ULARGE_INTEGER uli;
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        // FILETIME is in 100-ns ticks
        return static_cast<double>(uli.QuadPart) / 10000.0;
    }
#endif

    ProcessSample SampleSelf() {
        ProcessSample out{};
#ifdef _WIN32
        out.pid = GetCurrentProcessId();
        HANDLE hProc = GetCurrentProcess();
        FILETIME c, e, k, u;
        if (GetProcessTimes(hProc, &c, &e, &k, &u)) {
            out.kernel_ms = fileTimeToMs(k);
            out.user_ms = fileTimeToMs(u);
        }
        // Enumerate threads
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == out.pid) {
                        ThreadSample ts{};
                        ts.tid = te.th32ThreadID;
                        HANDLE hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                        if (hTh) {
                            FILETIME tc, tei, tk, tu;
                            if (GetThreadTimes(hTh, &tc, &tei, &tk, &tu)) {
                                ts.kernel_ms = fileTimeToMs(tk);
                                ts.user_ms = fileTimeToMs(tu);
                            }
                            CloseHandle(hTh);
                        }
                        out.thread_samples.push_back(ts);
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
#elif __linux__
        out.pid = static_cast<uint32_t>(getpid());
        // Process times from /proc/self/stat (utime, stime are in clock ticks)
        {
            std::ifstream f("/proc/self/stat");
            std::string tmp, comm; char state;
            uint64_t ppid, pgrp, session, tty_nr, tpgid;
            uint64_t flags, minflt, cminflt, majflt, cmajflt, utime, stime;
            // skip many fields, read up to utime, stime
            // format: pid (comm) state ... utime stime ...
            if (f) {
                f >> out.pid;
                f >> comm >> state;
                for (int i = 0; i < 10; i++) f >> tmp; // skip to minflt
                f >> minflt >> cminflt >> majflt >> cmajflt >> utime >> stime;
                long ticks = sysconf(_SC_CLK_TCK);
                out.user_ms = 1000.0 * static_cast<double>(utime) / ticks;
                out.kernel_ms = 1000.0 * static_cast<double>(stime) / ticks;
            }
        }
        // Threads: iterate /proc/self/task
        {
            DIR* d = opendir("/proc/self/task");
            if (d) {
                dirent* ent;
                while ((ent = readdir(d)) != nullptr) {
                    if (ent->d_name[0] == '.') continue;
                    ThreadSample ts{};
                    ts.tid = static_cast<uint32_t>(std::stoul(ent->d_name));
                    // per-thread times
                    std::ifstream ft(std::string("/proc/self/task/") + ent->d_name + "/stat");
                    if (ft) {
                        std::string tcomm; char tstate;
                        uint64_t utime, stime;
                        std::string skip;
                        ft >> ts.tid >> tcomm >> tstate;
                        for (int i = 0; i < 10; i++) ft >> skip;
                        uint64_t minflt, cminflt, majflt, cmajflt;
                        ft >> minflt >> cminflt >> majflt >> cmajflt >> utime >> stime;
                        long ticks = sysconf(_SC_CLK_TCK);
                        ts.user_ms = 1000.0 * static_cast<double>(utime) / ticks;
                        ts.kernel_ms = 1000.0 * static_cast<double>(stime) / ticks;
                    }
                    out.thread_samples.push_back(ts);
                }
                closedir(d);
            }
        }
#endif
        out.threads = out.thread_samples.size();
        out.taken_at = std::chrono::steady_clock::now();
        return out;
    }

    CpuDelta Diff(const ProcessSample& a, const ProcessSample& b) {
        CpuDelta d{};
        // Use (std::max)(...) to be robust even if min/max macros slip in
        d.proc_user_ms = (std::max)(0.0, b.user_ms - a.user_ms);
        d.proc_kernel_ms = (std::max)(0.0, b.kernel_ms - a.kernel_ms);
        // match threads by tid
        for (const auto& tb : b.thread_samples) {
            auto it = std::find_if(a.thread_samples.begin(), a.thread_samples.end(),
                [&](const ThreadSample& ta) { return ta.tid == tb.tid; });
            ThreadSample delta{};
            delta.tid = tb.tid;
            if (it != a.thread_samples.end()) {
                delta.user_ms = (std::max)(0.0, tb.user_ms - it->user_ms);
                delta.kernel_ms = (std::max)(0.0, tb.kernel_ms - it->kernel_ms);
            }
            else {
                // new thread
                delta.user_ms = tb.user_ms;
                delta.kernel_ms = tb.kernel_ms;
            }
            d.thread_deltas.push_back(delta);
        }
        return d;
    }

    void summarizeCSV(const std::string& path) {
        std::ifstream f(path);
        if (!f) {
            std::cerr << "PMU: Failed to open file: " << path << std::endl;
            return;
        }

        std::string header, proc_line, thread_header, line;
        if (!std::getline(f, header)) return;
        if (!std::getline(f, proc_line)) return;
        if (!std::getline(f, thread_header)) return;

        struct TRow {
            uint32_t tid;
            double user_ms;
            double kernel_ms;
        };

        std::vector<TRow> rows;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string tid_s, user_s, kernel_s;
            if (!std::getline(ss, tid_s, ',')) continue;
            if (!std::getline(ss, user_s, ',')) continue;
            if (!std::getline(ss, kernel_s, ',')) continue;

            TRow row;
            row.tid = static_cast<uint32_t>(std::stoul(tid_s));
            row.user_ms = std::stod(user_s);
            row.kernel_ms = std::stod(kernel_s);
            rows.push_back(row);
        }

        std::sort(rows.begin(), rows.end(),
            [](const TRow& a, const TRow& b) {
                return (a.user_ms + a.kernel_ms) > (b.user_ms + b.kernel_ms);
            });

        std::cout << "--- PMU Summary ---\n";
        std::cout << header << " => " << proc_line << "\n";
        for (const auto& r : rows) {
            std::cout << "tid=" << r.tid
                << " cpu_ms=" << std::fixed << std::setprecision(3) << (r.user_ms + r.kernel_ms)
                << " (user=" << r.user_ms << ", kernel=" << r.kernel_ms << ")\n";
            // Build a compact, AI-friendly summary
            size_t topN = std::min<size_t>(rows.size(), 5);
            std::ostringstream ai;
            ai << "PMU Summary: threads=" << rows.size() << "; top " << topN << " by cpu_ms: ";
            for (size_t i = 0; i < topN; ++i) {
                double cpu = rows[i].user_ms + rows[i].kernel_ms;
                ai << "[tid=" << rows[i].tid << " cpu_ms=" << std::fixed << std::setprecision(3) << cpu << "]";
                if (i + 1 < topN) ai << " ";
            }
            OmniAIManager::setRecentPmuSummary(ai.str());
        }

    }

    // ==============================
    // ADDITIONS (no removals)
    // ==============================

    // Logical CPU count
    static inline size_t logicalCpuCount() {
#ifdef _WIN32
        // Prefer GetActiveProcessorCount if available (Win7+)
        typedef DWORD(WINAPI* GetAPC)(WORD);
        HMODULE h = GetModuleHandleA("kernel32.dll");
        if (h) {
            auto p = (GetAPC)GetProcAddress(h, "GetActiveProcessorCount");
            if (p) {
                DWORD cnt = p(0xFFFF); // ALL_PROCESSOR_GROUPS
                if (cnt > 0) return (size_t)cnt;
            }
        }
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return si.dwNumberOfProcessors ? (size_t)si.dwNumberOfProcessors : 1;
#elif __linux__
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        return n > 0 ? (size_t)n : 1;
#else
        return 1;
#endif
    }

    // Elapsed wall time between two samples (ms)
    static inline double elapsedMs(const ProcessSample& a, const ProcessSample& b) {
        if (b.taken_at <= a.taken_at) return 0.0;
        auto d = std::chrono::duration_cast<std::chrono::milliseconds>(b.taken_at - a.taken_at).count();
        return d > 0 ? static_cast<double>(d) : 0.0;
    }

    // CPU% summary container
    struct CpuPercentages {
        double proc_ms = 0.0;
        double proc_pct = 0.0;
        std::vector<std::pair<uint32_t, double>> thread_ms;  // (tid, cpu_ms)
        std::vector<std::pair<uint32_t, double>> thread_pct; // (tid, cpu_pct)
    };

    // Compute deltas and normalize to CPU%
    static inline CpuPercentages ComputeCpuPercentages(const ProcessSample& a, const ProcessSample& b) {
        CpuPercentages out{};
        const double dt_ms = elapsedMs(a, b);
        const double ncpu = static_cast<double>(logicalCpuCount());
        CpuDelta d = Diff(a, b);

        out.proc_ms = d.proc_user_ms + d.proc_kernel_ms;
        if (dt_ms > 0.0 && ncpu > 0.0) {
            out.proc_pct = (out.proc_ms / (dt_ms * ncpu)) * 100.0;
            if (out.proc_pct < 0.0) out.proc_pct = 0.0;
        }

        out.thread_ms.reserve(d.thread_deltas.size());
        out.thread_pct.reserve(d.thread_deltas.size());
        for (const auto& t : d.thread_deltas) {
            double ms = (t.user_ms + t.kernel_ms);
            out.thread_ms.emplace_back(t.tid, ms);
            double pct = 0.0;
            if (dt_ms > 0.0 && ncpu > 0.0) {
                pct = (ms / (dt_ms * ncpu)) * 100.0;
                if (pct < 0.0) pct = 0.0;
            }
            out.thread_pct.emplace_back(t.tid, pct);
        }
        std::sort(out.thread_ms.begin(), out.thread_ms.end(),
            [](const auto& A, const auto& B) { return A.second > B.second; });
        std::sort(out.thread_pct.begin(), out.thread_pct.end(),
            [](const auto& A, const auto& B) { return A.second > B.second; });
        return out;
    }

    // Resolve thread names for current process (best-effort)
    static inline std::map<uint32_t, std::string> GetThreadNamesSelf() {
        std::map<uint32_t, std::string> names;
#ifdef _WIN32
        DWORD pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te; te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == pid) {
                        HANDLE hTh = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                        if (!hTh) {
                            hTh = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                        }
                        if (hTh) {
                            typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);
                            HMODULE h = GetModuleHandleA("Kernel32.dll");
                            if (!h) h = GetModuleHandleA("KernelBase.dll");
                            std::string name;
                            if (h) {
                                auto p = (GetThreadDescriptionFn)GetProcAddress(h, "GetThreadDescription");
                                if (p) {
                                    PWSTR wname = nullptr;
                                    if (SUCCEEDED(p(hTh, &wname)) && wname) {
                                        int len = WideCharToMultiByte(CP_UTF8, 0, wname, -1, nullptr, 0, nullptr, nullptr);
                                        if (len > 0) {
                                            std::string utf8(len - 1, '\0');
                                            WideCharToMultiByte(CP_UTF8, 0, wname, -1, utf8.data(), len, nullptr, nullptr);
                                            name = std::move(utf8);
                                        }
                                        LocalFree(wname);
                                    }
                                }
                            }
                            if (!name.empty()) {
                                names.emplace(te.th32ThreadID, std::move(name));
                            }
                            CloseHandle(hTh);
                        }
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
#elif __linux__
        DIR* d = opendir("/proc/self/task");
        if (d) {
            dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                uint32_t tid = static_cast<uint32_t>(std::stoul(ent->d_name));
                std::ifstream comm(std::string("/proc/self/task/") + ent->d_name + "/comm");
                if (comm) {
                    std::string n;
                    std::getline(comm, n);
                    if (!n.empty() && n.back() == '\n') n.pop_back();
                    if (!n.empty()) names.emplace(tid, std::move(n));
                }
            }
            closedir(d);
        }
#endif
        return names;
    }

    // Build a concise top-N summary over a delta
    static inline std::string BuildTopThreadSummary(const ProcessSample& a,
        const ProcessSample& b,
        size_t topN) // default lives in PMU.h
    {
        CpuPercentages pct = ComputeCpuPercentages(a, b);
        const double dt = elapsedMs(a, b);
        const size_t ncpu = logicalCpuCount();
        auto names = GetThreadNamesSelf();

        // Build list: (tid, cpu_ms, cpu_pct, name)
        std::vector<std::tuple<uint32_t, double, double, std::string>> top;
        top.reserve(pct.thread_ms.size());

        std::map<uint32_t, double> pctByTid;
        for (const auto& p : pct.thread_pct) {
            pctByTid[p.first] = p.second;
        }

        for (const auto& m : pct.thread_ms) {
            uint32_t tid = m.first;
            double ms = m.second;
            double p = pctByTid.count(tid) ? pctByTid[tid] : 0.0;
            std::string nm = names.count(tid) ? names[tid] : std::string{};
            top.emplace_back(tid, ms, p, std::move(nm));
        }

        std::sort(top.begin(), top.end(),
            [](const auto& A, const auto& B) {
                return std::get<1>(A) > std::get<1>(B);
            });

        if (topN > top.size()) {
            topN = top.size();
        }

        std::ostringstream os;
        os << "PMU Live: dt_ms=" << std::fixed << std::setprecision(0) << dt
            << " ncpu=" << ncpu
            << " proc_ms=" << std::fixed << std::setprecision(3) << pct.proc_ms
            << " proc_pct=" << std::fixed << std::setprecision(2) << pct.proc_pct
            << "; top " << topN << ": ";

        for (size_t i = 0; i < topN; ++i) {
            auto& t = top[i];
            uint32_t tid = std::get<0>(t);
            double ms = std::get<1>(t);
            double p = std::get<2>(t);
            const std::string& nm = std::get<3>(t);

            os << "[tid=" << tid;
            if (!nm.empty()) {
                os << " name=\"" << nm << "\"";
            }
            os << " cpu_ms=" << std::fixed << std::setprecision(3) << ms
                << " cpu%=" << std::fixed << std::setprecision(2) << p << "]";
            if (i + 1 < topN) {
                os << " ";
            }
        }

        return os.str();
    }

    // Convenience printer for a single delta
    void PrintDelta(const ProcessSample& a, const ProcessSample& b, size_t topN)
    {
        std::cout << BuildTopThreadSummary(a, b, topN) << std::endl;
    }

} // namespace PMU

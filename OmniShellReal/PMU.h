//=============================================================
// PMU.h
//=============================================================
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <optional>
#include <functional>
#include <atomic>

namespace PMU {

    // Represents a single thread's CPU time sample
    struct ThreadSample {
        uint32_t tid{ 0 };
        double user_ms{ 0.0 };
        double kernel_ms{ 0.0 };
        std::optional<int> cpu_affinity; // logical CPU id if known
    };

    // Control flag for live monitor loop (namespace‑scope)
    extern std::atomic_bool g_pmuStopFlag;

    // Represents the process CPU time sample with per-thread details
    struct ProcessSample {
        uint32_t pid{ 0 };
        double user_ms{ 0.0 };
        double kernel_ms{ 0.0 };
        size_t threads{ 0 };
        std::vector<ThreadSample> thread_samples;
        std::chrono::steady_clock::time_point taken_at;
    };

    // CPU time delta between two samples
    struct CpuDelta {
        double proc_user_ms{ 0.0 };
        double proc_kernel_ms{ 0.0 };
        std::vector<ThreadSample> thread_deltas; // per-thread delta
    };

    // Take a single snapshot of current process and its threads
    ProcessSample SampleSelf();

    // Compute delta CPU times between two snapshots
    CpuDelta Diff(const ProcessSample& a, const ProcessSample& b);

    // Summarize CSV file of samples (implementation-specific)
    void summarizeCSV(const std::string& path);

    // Continuously monitor current process at given interval
    //  - interval: how often to sample
    //  - topN: number of busiest threads to include in summaries
    //  - onSummary: optional callback to consume formatted summaries
    //  - stopFlag: optional atomic flag to signal graceful stop
    void MonitorSelf(std::chrono::milliseconds interval,
        size_t topN = 5,
        std::function<void(const std::string&)> onSummary = {},
        std::atomic_bool* stopFlag = nullptr);

    // Pretty-print a delta between two samples
    void PrintDelta(const ProcessSample& a,
        const ProcessSample& b,
        size_t topN = 5);

    // Build a concise top-N summary over a delta
    std::string BuildTopThreadSummary(const ProcessSample& a,
        const ProcessSample& b,
        size_t topN = 5);

    // Retrieve last summary string (if stored globally)
    std::string getRecentPmuSummary();

} // namespace PMU

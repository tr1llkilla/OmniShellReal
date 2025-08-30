Copyright © 2025 Cadell Richard Anderson

// =================================================================
// DaemonMonitor.cpp
// =================================================================
#include "DaemonMonitor.h"
#include "SensorManager.h"      
#include "OmniAIManager.h"     
#include "ShellExecutor.h"
#include "CommandRouter.h"
#include "TileAnalytics.h"
#include <iostream>
#include <chrono>
#include <iomanip>

DaemonMonitor::DaemonMonitor() : isRunning(false) {}

DaemonMonitor::~DaemonMonitor() {
    stop();
}

void DaemonMonitor::start(const ConfigState& config) {
    if (isRunning) {
        std::cout << "[Daemon] Monitor is already running." << std::endl;
        return;
    }
    isRunning = true;
    monitorThread = std::thread(&DaemonMonitor::monitorLoop, this, config);
    std::cout << "[Daemon] AI maintenance monitor started." << std::endl;
}

void DaemonMonitor::stop() {
    if (isRunning) {
        isRunning = false;
        if (monitorThread.joinable()) {
            monitorThread.join();
        }
        std::cout << "[Daemon] AI maintenance monitor stopped." << std::endl;
    }
}

std::string DaemonMonitor::getStatus() const {
    if (isRunning) {
        return "[Daemon] Status: Active.";
    }
    return "[Daemon] Status: Inactive.";
}

// NEW: properly defined as a class member, outside monitorLoop
void DaemonMonitor::captureTileTelemetry() {
    std::vector<uint16_t> chunks = { 0xDEF0,0x9ABC,0x5678,0x1234,0xDEF0,0x9ABC,0x5678,0x1234 };

    TileRunConfig tcfg;
    tcfg.rows = 64;
    tcfg.cols = 64;
    tcfg.out_dir = "telemetry";
    tcfg.run_tag = "daemon_capture";

    auto summary = TileAnalytics::RunFromChunks(chunks, tcfg);

    std::cout << "Tile telemetry written to: " << summary.csv_path << std::endl;
    for (const auto& heat : summary.heatmaps) {
        std::cout << "Heatmap: " << heat << std::endl;
    }
}

// NEW: choose a profile based on telemetry
SamplingProfile DaemonMonitor::decideProfile(
    const std::vector<SensorData>& sensors,
    const ConfigState& currentConfig)
{
    bool hot_cpu = false;
    bool low_battery = false;

    for (const auto& s : sensors) {
        if (s.id == "thermal_cpu" && s.value > currentConfig.cpuThreshold) {
            hot_cpu = true;
        }
        if (s.id == "battery_pct" && s.value < currentConfig.batteryMinThreshold) {
            low_battery = true;
        }
    }

    if (hot_cpu) return SamplingProfile::HighQuality;
    if (low_battery) return SamplingProfile::FastPreview;
    return SamplingProfile::Balanced;
}

void DaemonMonitor::monitorLoop(ConfigState config) {
    while (isRunning) {
        std::cout << "\n[Daemon] Running periodic check..." << std::endl;
        auto sensors = SensorManager::listSensors();

        // Apply adaptive sampling profile
        auto chosen = decideProfile(sensors, config);
        OmniAIManager::applySamplingProfile(chosen, config);
        std::cout << "[Daemon] Applied profile: " << static_cast<int>(chosen) << std::endl;

        auto plan = OmniAIManager::analyzeAndRecommend(sensors, config);

        bool hot_cpu = false;
        for (const auto& s : sensors) {
            if (s.id == "thermal_cpu" && s.value > config.cpuThreshold) {
                hot_cpu = true;
                break;
            }
        }
        if (hot_cpu) {
            std::cout << "[Daemon] CPU hot, running tile probe with PMU...\n";
            // Minimal buffer seeded by chunks
            std::vector<uint16_t> chunks = { 0xDEF0,0x9ABC,0x5678,0x1234,0xDEF0,0x9ABC,0x5678,0x1234 };
            TileRunConfig tcfg;
            tcfg.rows = 128; tcfg.cols = 128;
            tcfg.target_time_ms = config.tileTargetTimeMs;
            tcfg.high_prio_fraction = config.tileHighPrioFraction;
            tcfg.overlap_h = config.tileOverlapH;
            tcfg.overlap_w = config.tileOverlapW;
            tcfg.out_dir = config.tileOutDir;
            tcfg.run_tag = "daemon";
            auto summary = TileAnalytics::RunFromChunks(chunks, tcfg);
            std::cout << "[Daemon] Tile probe done: wall=" << std::fixed << std::setprecision(3)
                << summary.wall_ms << " ms, csv=" << summary.csv_path << "\n";
        }

        if (!plan.empty() && !(plan.size() == 1 && plan[0].description == "System appears nominal.")) {
            std::cout << "[Daemon] AI has recommended a repair plan. Executing..." << std::endl;
            for (const auto& step : plan) {
                std::cout << "  - Executing Step: " << step.description << std::endl;
                std::string result;
                switch (step.shell) {
                case ShellType::CMD:
                    result = ShellExecutor::run(step.command);
                    break;
                case ShellType::POWERSHELL:
                    result = ShellExecutor::runPowerShell(step.command);
                    break;
                case ShellType::OMNI:
                {
                    CommandRouter router; // Create a temporary router instance
                    result = router.dispatch(step.command);
                }
                break;
                case ShellType::BASH:
                    result = ShellExecutor::run(step.command);
                    break;
                }
                std::cout << "    Result:\n" << result << std::endl;
            }
        }
        else {
            std::cout << "[Daemon] AI Analysis: System nominal." << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(config.daemonIntervalSeconds));
    }
}

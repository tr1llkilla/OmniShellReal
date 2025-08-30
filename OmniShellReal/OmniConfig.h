Copyright © 2025 Cadell Richard Anderson

// =================================================================
// OmniConfig.h
// =================================================================
#pragma once
#include <string>
#include <vector>
#include <utility>

// Holds all settings loaded from OmniConfig.xml
struct ConfigState {
    bool tileHeatmapUseEntropy = true;
    bool enableDiskLog = true;
    bool allowRoot = true;
    bool monitorSensors = true;
    int  cpuThreshold = 90;
    int  batteryMinThreshold = 20;
    double entropyThreshold = 7.5;
    int  daemonIntervalSeconds = 30;
    double tileTargetTimeMs = 0.8;
    double tileHighPrioFraction = 0.25;
    int  tileOverlapH = 1;
    int  tileOverlapW = 1;

    std::string tileOutDir = "./telemetry";
    std::string defaultQuarantineDir = "./quarantine";
    std::string defaultReportDir = "./reports";

    // NEW: name + raw byte pattern for signature matching
    std::vector<std::pair<std::string, std::vector<unsigned char>>> signaturePatterns;
};

namespace OmniConfigNS {
    // Load config file into provided ConfigState
    bool load(const std::string& configFilePath, ConfigState& outConfig);

    // NEW: tries an explicit path first, then absolute, exe-relative, working dir
    bool load_with_fallback(ConfigState& outConfig,
        const std::string& explicitPath = "");
}

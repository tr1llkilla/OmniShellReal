// OmniAIManager.cpp
// UPDATED to use the full suite of sensor data for more intelligent analysis.

#include "OmniAIManager.h"
#include "SensorManager.h"
#include "OmniConfig.h"
#include "TileAnalytics.h"

#include <sstream>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <optional>
#include <thread>
#include <chrono>

// Allow referencing the global config instance that's defined in OmniConfig.cpp
extern ConfigState appConfig;

// --------------------------------------------
// Globals and caches
// --------------------------------------------
static AiMode g_mode = AiMode::Verbose;
static std::string g_recentPmuSummary;
static std::string g_recentTilesSummary;

// Static storage for new streaming & hooks features
std::vector<StreamSubscriber> OmniAIManager::subscribers;
std::unordered_map<LifecycleEvent, std::vector<std::function<void()>>> OmniAIManager::hooks;

// --------------------------------------------
// Lightweight utils
// --------------------------------------------
static std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    auto b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

static std::string applyModePrefix(const std::string& s) {
    switch (g_mode) {
    case AiMode::Concise: return "(concise) " + s;
    case AiMode::Debug:   return "(debug) " + s;
    default:              return "(verbose) " + s;
    }
}

// --------------------------------------------
// Mode and caches
// --------------------------------------------
void OmniAIManager::setMode(AiMode m) { g_mode = m; }
AiMode OmniAIManager::mode() { return g_mode; }

void OmniAIManager::setRecentPmuSummary(const std::string& s) { g_recentPmuSummary = s; }
void OmniAIManager::setRecentTilesSummary(const std::string& s) { g_recentTilesSummary = s; }
std::string OmniAIManager::recentPmuSummary() { return g_recentPmuSummary; }
std::string OmniAIManager::recentTilesSummary() { return g_recentTilesSummary; }

// --------------------------------------------
// Internal helpers that take ConfigState (keeps original logic intact)
// These are non-member helpers so they don't require header changes.
// --------------------------------------------
static std::string buildContextSnapshot(const ConfigState& cfg) {
    std::ostringstream os;
    os << "[Config]\n";
    os << "cpuThreshold=" << cfg.cpuThreshold << "\n";
    os << "batteryMinThreshold=" << cfg.batteryMinThreshold << "\n";
    os << "entropyThreshold=" << cfg.entropyThreshold << "\n";
    os << "\n[LiveSensors]\n";
    auto sensors = SensorManager::listSensors();
    for (const auto& s : sensors) {
        os << s.id << "=" << s.value << " " << s.unit << "\n";
    }
    return os.str();
}

static std::string buildContext(const ConfigState& cfg) {
    std::ostringstream out;
    out << "--- OmniAI Context ---\n";
    out << "Config Flags:\n";
    out << "  cpuThreshold=" << cfg.cpuThreshold << "\n";
    out << "  batteryMinThreshold=" << cfg.batteryMinThreshold << "\n";
    out << "  entropyThreshold=" << cfg.entropyThreshold << "\n";
    out << "\nLive Sensors:\n";
    auto sensors = SensorManager::listSensors();
    for (const auto& s : sensors) {
        out << "  " << s.label << ": " << s.value << " " << s.unit << "\n";
    }
    return out.str();
}

// --------------------------------------------
// Public member functions (no-arg versions call the helpers using the global config)
// These match the original header declarations (no signature changes required).
// --------------------------------------------
std::string OmniAIManager::getContextSnapshot() {
    return buildContextSnapshot(appConfig);
}

std::string OmniAIManager::getContext() {
    return buildContext(appConfig);
}

std::string OmniAIManager::listModels() {
    return std::string()
        + "--- Available Models ---\n"
        + "  [local] ollama:mistral:quant\n"
        + "  [local] ollama:phi3:full\n"
        + "  [remote] cloud:groq:mixtral\n"
        + "  [remote] cloud:openrouter:claude-haiku\n";
}

// --------------------------------------------
// Heuristic assessment when no model is wired
// --------------------------------------------
std::string OmniAIManager::heuristicAssessTopRisks(const std::string& ctxBlob) {
    bool sensorsMissing = ctxBlob.find("[LiveSensors]") != std::string::npos &&
        trim(ctxBlob.substr(ctxBlob.find("[LiveSensors]"))) == "[LiveSensors]";
    bool defaultCfg = ctxBlob.find("cpuThreshold=90") != std::string::npos;

    std::ostringstream os;
    os << "Top 3 risks and actions:\n";
    int i = 1;
    if (defaultCfg) {
        os << i++ << ". Risk: Default config in use (OmniConfig.xml missing).\n"
            << "   Action: Create OmniConfig.xml with tuned thresholds; enable sensor providers.\n";
    }
    if (sensorsMissing) {
        os << i++ << ". Risk: No live sensors detected.\n"
            << "   Action: Verify sensor drivers; run omni:sensor_list; enable polling in config.\n";
    }
    os << i++ << ". Risk: Unknown workload profile.\n"
        << "   Action: Capture PMU before/after and run omni:pmu_diff; then omni:explain the diff for mitigations.\n";
    return os.str();
}

// --------------------------------------------
// Query APIs
// --------------------------------------------
std::string OmniAIManager::query(const std::string& userQ) {
    if (userQ.find("what is a bool") != std::string::npos) {
        if (g_mode == AiMode::Concise) return "A bool is true/false; typically 1 byte; use for flags.";
        return "A bool represents a logical value: true or false. Use to model on/off, success/failure, or feature flags.";
    }
    return applyModePrefix("I need context to be precise. Try --with-context or omni:ctx first.");
}

std::string OmniAIManager::queryWithContext(const std::string& userQ, const std::string& ctxBlob) {
    if (userQ.find("top 3 risks") != std::string::npos) {
        return heuristicAssessTopRisks(ctxBlob);
    }
    if (userQ.find("Explain the thread deltas") != std::string::npos) {
        std::ostringstream os;
        os << "PMU thread deltas:\n"
            << "- Interpret user/kernel spikes per TID, focus on outliers > 3 standard deviations.\n"
            << "Mitigation:\n"
            << "1) Pin noisy threads to isolated cores or reduce affinity overlap.\n"
            << "2) Throttle the hottest codepath; add sampling profiler over 30s.\n";
        return os.str();
    }
    std::ostringstream os;
    os << "Context summary received (" << ctxBlob.size() << " bytes).\n"
        << "Request: " << userQ << "\n"
        << "Next actions:\n"
        << "- Validate sensors (omni:sensor_list) and capture PMU before/after.\n"
        << "- Summarize logs: omni:log:sum <path> --errors-only.\n";
    return os.str();
}

// --------------------------------------------
// Explain / Generate / Summarize
// --------------------------------------------
std::string OmniAIManager::explain(const std::string& targetText) {
    std::ostringstream os;
    os << "Explanation:\n";
    if (targetText.rfind("omni:", 0) == 0) {
        os << "- This is an OmniShell command. It manipulates telemetry or diagnostics.\n"
            << "Advice:\n"
            << "- Use --help on the command; capture output; rerun omni:explain with logs.\n";
    }
    else {
        os << "- Summary: " << targetText.substr(0, std::min<size_t>(targetText.size(), 240));
        if (targetText.size() > 240) os << "...";
        os << "\nSignals to track: ERROR/WARN, exit codes, and long durations.\n";
    }
    return os.str();
}

std::string OmniAIManager::generate(const std::string& goal, bool dryRunRequested) {
    std::ostringstream dry, run;
    dry << "# Dry-Run plan for goal: " << goal << "\n"
        << "- Capture context (omni:ctx)\n"
        << "- PMU before/after; diff\n"
        << "- Summarize logs; propose fix checklist\n";
    run << "# Runnable\n"
        << "omni:ctx\n"
        << "omni:pmu_save before.csv\n"
        << "<run workload>\n"
        << "omni:pmu_save after.csv\n"
        << "omni:pmu_diff before.csv after.csv\n"
        << "omni:log:sum logs\\app.log --errors-only\n";
    return dryRunRequested ? dry.str() : dry.str() + "\n" + run.str();
}

std::string OmniAIManager::summarize(const std::string& text) {
    std::istringstream is(text);
    std::string line;
    std::vector<std::string> errs;

    while (std::getline(is, line)) {
        std::string L = line;
        std::transform(L.begin(), L.end(), L.begin(), ::toupper);
        if (L.find("ERROR") != std::string::npos || L.find("WARN") != std::string::npos) {
            errs.push_back(trim(line));
        }
    }

    std::ostringstream os;
    os << "Log summary:\n"
        << "- Lines analyzed: " << (int)std::count(text.begin(), text.end(), '\n') + 1 << "\n"
        << "- Error/Warning lines: " << errs.size() << "\n";
    for (size_t i = 0; i < std::min<size_t>(errs.size(), 20); ++i) {
        os << "  - " << errs[i] << "\n";
    }
    os << "Next actions:\n"
        << "- Address highest-frequency errors first; rerun omni:log:sum --since=1h to confirm fixes.\n";
    return os.str();
}

// --------------------------------------------
// Diagnose and recommend from sensors
// --------------------------------------------
std::vector<RepairStep> OmniAIManager::analyzeAndRecommend(const std::vector<SensorData>& sensors,
    const ConfigState& config) {
    std::vector<RepairStep> steps;

    auto shellFor = [](const std::string& cmd) -> ShellType {
        if (cmd.rfind("omni:", 0) == 0) return ShellType::OMNI;
#ifdef _WIN32
        if (cmd.rfind("powershell", 0) == 0 || cmd.rfind("pwsh", 0) == 0) return ShellType::POWERSHELL;
        return ShellType::CMD;
#else
        return ShellType::BASH;
#endif
        };

    // --- NEW: More intelligent, correlational analysis ---

    // First, gather critical sensor states
    std::optional<double> max_temp;
    std::optional<double> min_fan_rpm;
    std::optional<double> high_cpu_load;
    std::optional<double> low_disk_free;
    std::optional<double> high_mem_usage;

    for (const auto& s : sensors) {
        if (s.type == "thermal" && s.status == SensorStatus::CRITICAL) {
            if (!max_temp || s.value > *max_temp) max_temp = s.value;
        }
        if (s.type == "fan") {
            if (!min_fan_rpm || s.value < *min_fan_rpm) min_fan_rpm = s.value;
        }
        if (s.type == "cpu" && s.status == SensorStatus::WARN) {
            if (!high_cpu_load || s.value > *high_cpu_load) high_cpu_load = s.value;
        }
        if (s.type == "disk" && s.status == SensorStatus::WARN) {
            if (!low_disk_free || s.value < *low_disk_free) low_disk_free = s.value;
        }
        if (s.type == "memory" && s.status == SensorStatus::WARN) {
            if (!high_mem_usage || s.value > *high_mem_usage) high_mem_usage = s.value;
        }
    }

    // Now, check for correlations
    if (max_temp && min_fan_rpm && *min_fan_rpm < 100) {
        steps.push_back({ "Critical temperature detected with a non-functioning fan.",
                          "echo Check physical fan connection and for obstructions.",
                          shellFor("echo") });
        steps.push_back({ "Attempting to identify processes causing high thermal load.",
                          "omni:diagnose processes",
                          ShellType::OMNI });
    }
    else if (max_temp) {
        steps.push_back({ "High temperature detected: " + std::to_string(*max_temp) + "C",
                         "omni:diagnose processes",
                         ShellType::OMNI });
    }

    if (high_cpu_load && high_mem_usage) {
        steps.push_back({ "High CPU and Memory usage detected. System is under heavy load.",
                          "omni:diagnose processes",
                          ShellType::OMNI });
    }
    else if (high_cpu_load) {
        steps.push_back({ "High CPU usage detected: " + std::to_string(*high_cpu_load) + "%",
                         "omni:diagnose processes",
                         ShellType::OMNI });
    }
    else if (high_mem_usage) {
        steps.push_back({ "High Memory usage detected: " + std::to_string(*high_mem_usage) + "%",
                         "omni:diagnose processes",
                         ShellType::OMNI });
    }

    if (low_disk_free) {
        std::string cmd;
#ifdef _WIN32
        cmd = "cleanmgr /sagerun:1";
#else
        cmd = "sudo journalctl --vacuum-size=500M && sudo apt-get autoremove -y";
#endif
        steps.push_back({ "Low disk space detected: " + std::to_string(*low_disk_free) + "% free.", cmd, shellFor(cmd) });
    }

    if (steps.empty()) {
        steps.push_back({ "System appears nominal.",
                          "echo All diagnostics passed.",
        #ifdef _WIN32
                          ShellType::CMD
        #else
                          ShellType::BASH
        #endif
            });
    }

    return steps;
}

// --------------------------------------------
// New features: streaming, lifecycle hooks, sampling profile
// --------------------------------------------

// Notify all token stream subscribers
void OmniAIManager::notifySubscribers(const std::string& token) {
    for (auto& sub : subscribers) {
        if (sub.onToken) sub.onToken(token);
    }
}

// Fire lifecycle hooks for a specific event
void OmniAIManager::fireHooks(LifecycleEvent evt) {
    auto it = hooks.find(evt);
    if (it == hooks.end()) return;
    for (auto& cb : it->second) {
        if (cb) cb();
    }
}

// Add a stream subscriber
void OmniAIManager::addStreamSubscriber(const StreamSubscriber& sub) {
    subscribers.push_back(sub);
}

// Clear all stream subscribers
void OmniAIManager::clearStreamSubscribers() {
    subscribers.clear();
}

// Register a lifecycle hook
void OmniAIManager::addLifecycleHook(LifecycleEvent evt, std::function<void()> hook) {
    hooks[evt].push_back(std::move(hook));
}

// Streaming token generation variant (non-blocking / incremental)
void OmniAIManager::streamGenerate(const std::string& goal,
    const ConfigState& cfg,
    bool dryRunRequested) {
    std::ostringstream dry, run;
    dry << "# Dry-Run plan for goal: " << goal << "\n"
        << "- Capture context (omni:ctx)\n"
        << "- PMU before/after; diff\n"
        << "- Summarize logs; propose fix checklist\n";
    run << "# Runnable\n"
        << "omni:ctx\n"
        << "omni:pmu_save before.csv\n"
        << "<run workload>\n"
        << "omni:pmu_save after.csv\n"
        << "omni:pmu_diff before.csv after.csv\n"
        << "omni:log:sum logs\\app.log --errors-only\n";

    std::string full = dry.str();
    if (!dryRunRequested) {
        full += "\n";
        full += run.str();
    }

    fireHooks(LifecycleEvent::BeforeGeneration);

    std::istringstream lines(full);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::string tok;
        bool first = true;
        while (words >> tok) {
            if (!first) {
                notifySubscribers(" ");
                fireHooks(LifecycleEvent::TokenEmitted);
            }
            notifySubscribers(tok);
            fireHooks(LifecycleEvent::TokenEmitted);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            first = false;
        }
        notifySubscribers("\n");
        fireHooks(LifecycleEvent::TokenEmitted);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    fireHooks(LifecycleEvent::AfterGeneration);
}

// Apply a predefined modular sampling profile to a config
void OmniAIManager::applySamplingProfile(SamplingProfile profile, ConfigState& cfg) {
    switch (profile) {
    case SamplingProfile::FastPreview:
        cfg.cpuThreshold = std::max(cfg.cpuThreshold, 90);
        cfg.batteryMinThreshold = std::min(cfg.batteryMinThreshold, 15);
        cfg.entropyThreshold = std::max(cfg.entropyThreshold, 0.60);
        break;

    case SamplingProfile::Balanced:
        cfg.cpuThreshold = std::min(std::max(cfg.cpuThreshold, 80), 88);
        cfg.batteryMinThreshold = std::min(std::max(cfg.batteryMinThreshold, 20), 25);
        cfg.entropyThreshold = std::min(std::max(cfg.entropyThreshold, 0.70), 0.75);
        break;

    case SamplingProfile::HighQuality:
        cfg.cpuThreshold = std::min(cfg.cpuThreshold, 75);
        cfg.batteryMinThreshold = std::max(cfg.batteryMinThreshold, 30);
        cfg.entropyThreshold = std::min(cfg.entropyThreshold, 0.85);
        break;

    default:
        break;
    }
}
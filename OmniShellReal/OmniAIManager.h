Copyright © 2025 Cadell Richard Anderson

//OmniAIManager.h

#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include "SensorManager.h"  // SensorData
#include "OmniConfig.h"     // ConfigState

// Verbosity/mode control for AI responses
enum class AiMode { Concise, Verbose, Debug };

// Shell selection for execution in RepairStep
enum class ShellType { CMD, POWERSHELL, OMNI, BASH };

struct RepairStep {
    std::string description;
    std::string command;
#ifdef _WIN32
    ShellType shell = ShellType::CMD;
#else
    ShellType shell = ShellType::BASH;
#endif
};

// --- New Additions for Streaming Decode & Hooks ---

// Subscriber for token streaming
struct StreamSubscriber {
    std::function<void(const std::string& token)> onToken;
};

// Lifecycle hook events
enum class LifecycleEvent { BeforeGeneration, AfterGeneration, TokenEmitted };

// Example sampling profile selector for modular sampling configs
enum class SamplingProfile { FastPreview, Balanced, HighQuality };

class OmniAIManager {
public:
    // Primary query interface
    static std::string query(const std::string& input);
    static std::string queryWithContext(const std::string& input, const std::string& context);

    // Context and summaries
    static std::string getContextSnapshot();
    static std::string getContext();

    // Model surfacing
    static std::string listModels();

    // Mode controls
    static void setMode(AiMode mode);
    static AiMode mode();

    // Summaries cache
    static std::string recentPmuSummary();
    static std::string recentTilesSummary();
    static void setRecentPmuSummary(const std::string& s);
    static void setRecentTilesSummary(const std::string& s);

    // Helpers
    static std::string summarize(const std::string& blob);
    static std::string generate(const std::string& goal, bool dryRun);
    static std::string explain(const std::string& blob);

    // Diagnostics from sensors -> recommended actions
    static std::vector<RepairStep> analyzeAndRecommend(const std::vector<SensorData>& sensors,
        const ConfigState& config);

    // --- New Public APIs for Streaming & Sampling ---
    static void addStreamSubscriber(const StreamSubscriber& sub);
    static void clearStreamSubscribers();
    static void addLifecycleHook(LifecycleEvent evt, std::function<void()> hook);

    // Streaming token generation variant (non-blocking / incremental)
    static void streamGenerate(const std::string& goal,
        const ConfigState& cfg,
        bool dryRunRequested);

    // Apply a predefined modular sampling profile to a config
    static void applySamplingProfile(SamplingProfile profile, ConfigState& cfg);

private:
    static std::string heuristicAssessTopRisks(const std::string& ctxBlob);

    // --- Internal helpers for new features ---
    static void notifySubscribers(const std::string& token);
    static void fireHooks(LifecycleEvent evt);

    static std::vector<StreamSubscriber> subscribers;
    static std::unordered_map<LifecycleEvent, std::vector<std::function<void()>>> hooks;
};

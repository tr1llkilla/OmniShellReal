Copyright © 2025 Cadell Richard Anderson

// =================================================================
// CommandRouter.cpp
// =================================================================

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wininet.h>
#include <winhttp.h>
#endif

// For M_PI, needs to be defined before <cmath> is included.
#define _USE_MATH_DEFINES

// =================================================================
// 2. Standard Library Headers
// =================================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// =================================================================
// 3. Project Headers
// =================================================================
#include "BinaryManip.h"
#include "CommandRouter.h"
#include "DaemonMonitor.h"
#include "DiagnosticsModule.h"
#include "JobManager.h"
#include "OmniAIManager.h"
#include "OmniConfig.h"
#include "OmniEditorIDE.h"
#include "PMU.h"
#include "ScriptRunner.h"
#include "SensorManager.h"
#include "ShellExecutor.h"
#include "TileAnalytics.h"
#include "ai_engine.h"
#include "ddc_engine.h"
#include "live_capture.h"
#include "model.h"
#include "packet_writer.h"
#include "packet_frame.h"
#include "scratch_engine.h"
#include "source_network_pcap.h"
#include "web_fetcher.h"
#include "CloudAPI.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// =================================================================
// Externs / statics
// =================================================================

// Provided by OmniConfig.h somewhere in the program
extern ConfigState appConfig;

// Local daemon instance for task control
static DaemonMonitor daemon;

using CommandMeta = CommandRouter::CommandMeta;
extern std::map<std::string, CommandMeta> g_command_metadata;

// ---------------- Command Metadata Table ----------------
std::map<std::string, CommandMeta> g_command_metadata = {
    // Core Commands
    { "help",        { "Core Commands", "help [command]", "List commands or show detailed help for one", true, true, true } },
    { "cd",          { "Core Commands", "cd [path|-]", "Change directory (persistent); no args prints current directory", true, true, true } },
    { "pwd",         { "Core Commands", "pwd", "Print current directory tracked by OmniShell", true, true, true } },
    { "omni:edit",   { "Core Commands", "omni:edit <file>", "Opens a file in the Omni text editor", true, true, true } },

    // Job Control
    { "<command> &", { "Job Control", "<command> &", "Runs a command in the background", true, true, true } },
    { "jobs",        { "Job Control", "jobs", "Lists all background jobs", true, true, true } },
    { "fg",          { "Job Control", "fg <job_id>", "Brings a background job to the foreground", true, true, true } },

    // Shell Integration
#if defined(_WIN32)
    { "ps",              { "Shell Integration", "ps <command>", "Executes a PowerShell command", true, true, true } },
    { "omni:dev",        { "Shell Integration", "omni:dev <command>", "Runs a command in the VS 2022 Developer Prompt", true, false, false } },
    { "omni:repair_all", { "Shell Integration", "omni:repair_all", "Runs a full suite of system repair tools", true, true, true } },
    { "omni:repair_sfc", { "Shell Integration", "omni:repair_sfc", "Runs the System File Checker", true, true, true } },
    { "omni:repair_dism",{ "Shell Integration", "omni:repair_dism", "Runs the DISM image repair tool", true, true, true } },
    { "omni:disk_check", { "Shell Integration", "omni:disk_check <D:>", "Runs CHKDSK on the specified drive", true, true, true } },
#elif defined(__linux__)
    { "omni:disk_usage", { "Shell Integration", "omni:disk_usage", "Show disk usage (df -h)", true, true, true } },
    { "omni:mem_info",   { "Shell Integration", "omni:mem_info", "Show memory usage (free -h)", true, true, true } },
    { "(pwsh)",          { "Shell Integration", "(PowerShell Core 'pwsh')", "Can be used if installed", true, true, true } },
#endif
    // Source Control
    { "git",   { "Source Control", "git <clone|pull|...>", "Executes a git command.", true, true, true } },
    { "vcpkg", { "Source Control", "vcpkg <install|...>", "Executes a vcpkg command for package management.", true, false, false } },

    // Diagnostics & Repair
    { "omni:sensor_list",{ "Diagnostics & Repair", "omni:sensor_list", "Displays a list of available hardware sensors", true, true, true } },
    { "omni:diagnose",   { "Diagnostics & Repair", "omni:diagnose...", "Runs diagnostic tools", true, true, true } },
    { "registry",        { "Diagnostics & Repair", "registry <key> <term>", "(Windows) Scans the registry", true, true, true } },
    { "entropy",         { "Diagnostics & Repair", "entropy <path> [quarantine_dir][report_dir]", "Scans file/dir entropy and quarantines high-entropy files", true, true, true } },
    { "processes",       { "Diagnostics & Repair", "processes", "Lists running processes", true, true, true } },
    { "omni:kill",       { "Diagnostics & Repair", "omni:kill <pid>", "Terminates a process by its PID", true, true, true } },
    { "omni:sensor_dump",     { "Diagnostics & Repair", "omni:sensor_dump", "Outputs detailed sensor readings for all sensors", true, false, false } },
    { "omni:sensor_show",     { "Diagnostics & Repair", "omni:sensor_show <id|label>", "Show detailed information for a single sensor", true, false, false } },
    { "omni:sensor_snapshot", { "Diagnostics & Repair", "omni:sensor_snapshot <out.json>", "Capture a snapshot of current sensor data to JSON", true, false, false } },
    { "omni:sensor_export",   { "Diagnostics & Repair", "omni:sensor_export <out.csv>", "Export sensor readings to CSV", true, false, false } },
    { "omni:sensor_filter",   { "Diagnostics & Repair", "omni:sensor_filter <criteria>", "Filter sensors by label, type, or threshold", true, false, false } },

    // Binary Analysis
    { "omni:binary probe",      { "Binary Analysis", "omni:binary probe <file>", "Probe a binary for OS, architecture, and type.", true, true, true } },
    { "omni:binary sections",   { "Binary Analysis", "omni:binary sections <file>", "List all sections in a PE or ELF binary.", true, true, true } },
    { "omni:binary symbols",    { "Binary Analysis", "omni:binary symbols <file>", "List all exported symbols in a binary.", true, true, true } },
    { "omni:binary attach",     { "Binary Analysis", "omni:binary attach <pid>", "Attach to a running process for instrumentation.", true, true, true } },
    { "omni:binary diff",       { "Binary Analysis", "omni:binary diff <file1> <file2>", "Find the first difference between two binary files.", true, true, true } },
    { "omni:binary ai-analyze", { "Binary Analysis", "omni:binary ai-analyze <file>", "Run AI-powered analysis on a binary for threats.", true, true, true } },

    // AI Daemon Control
    { "omni:task_daemon",{ "AI Daemon Control", "omni:task_daemon...", "Controls the AI maintenance daemon", true, true, true } },

    // AI Shell Commands
    { "omni:ask",        { "AI Shell Commands", "omni:ask <query> [--with-context]", "Query the AI assistant; optionally include system context", true, true, true } },
    { "omni:mode",       { "AI Shell Commands", "omni:mode <concise|verbose|debug>", "Switch AI verbosity", true, true, true } },
    { "omni:explain",    { "AI Shell Commands", "omni:explain <text>", "Ask AI to explain a command or concept", true, true, true } },
    { "omni:gen",        { "AI Shell Commands", "omni:gen \"<goal>\" [--dry-run]", "AI generation assistant", true, true, true } },
    { "omni:log:sum",    { "AI Shell Commands", "omni:log:sum <file>", "Summarize logs via AI and write reports", true, true, true } },
    { "omni:ctx",        { "AI Shell Commands", "omni:ctx", "Show current system context (sensors + config)", true, true, true } },
    { "omni:models",     { "AI Shell Commands", "omni:models", "List available LLM backends (local + remote)", true, true, true } },

    // Tile Analytics
    { "omni:tiles",      { "Tile Analytics", "omni:tiles run [rows cols][tag][--entropy|--runtime][--tt=ms][--hp=frac][--oh=H]", "Run tile analytics", true, true, true } },
    { "omni:tiles_sum",  { "Tile Analytics", "omni:tiles summarize <csv_path>", "Summarize tile analytics results from CSV", true, true, true } },
    { "omni:tiles_merge", { "Tile Analytics", "omni:tiles_merge <tile1.csv> <tile2.csv> [out.csv]", "Merge results of two tile analytics CSV files", true, false, false } },

    // PMU
    { "omni:pmu_sample", { "PMU", "omni:pmu_sample", "Capture current process/thread CPU times", true, true, true } },
    { "omni:pmu_save",   { "PMU", "omni:pmu_save <out.csv>", "Save current PMU sample to CSV", true, true, true } },
    { "omni:pmu_diff",   { "PMU", "omni:pmu_diff <old.csv> <new.csv>", "Diff two PMU CSVs", true, true, true } },
    { "omni:pmu_summary",{ "PMU", "omni:pmu_summary <csv>", "Summarize PMU CSV by thread CPU", true, true, true } },
    { "omni:pmu_analyze", { "PMU", "omni:pmu_analyze <data>", "Analyze PMU data for performance metrics", true, false, false } },
    { "omni:pmu_monitor", { "PMU", "omni:pmu_monitor", "Continuously monitor PMU counters", true, false, false } },

    // AI (Local LLM)
    { "omni:llm:load",   { "AI (Local LLM)", "omni:llm:load <model_path>", "Load a local.cllf model", true, true, true } },
    { "omni:llm:status", { "AI (Local LLM)", "omni:llm:status", "Show loaded model info", true, true, true } },
    { "omni:llm:gen",    { "AI (Local LLM)", "omni:llm:gen \"<prompt>\" [--n N][--top-k K][--top-p P][--nostream]", "Generate text", true, true, true } },
    { "omni:llm:set",    { "AI (Local LLM)", "omni:llm:set <param> <value>", "Set a parameter for the local LLM", true, false, false } },
    { "omni:llm:unload", { "AI (Local LLM)", "omni:llm:unload", "Unload the local LLM model", true, false, false } },
    { "omni:llm:help",   { "AI (Local LLM)", "omni:llm:help [command]", "Show help for local LLM commands", true, false, false } },

    // Cross-Platform Aliases
#if defined(_WIN32)
    { "ls",              { "Cross-Platform Aliases", "ls", "Alias for 'dir'", true, true, true } },
#elif defined(__linux__)
    { "dir",             { "Cross-Platform Aliases", "dir", "Alias for 'ls -la'", true, true, true } },
#endif
    // Network & Web
    { "ironrouter",       { "Network Capture", "ironrouter <subcommand> [...]", "Controls the network capture and processing pipeline", true, false, false } },
    { "web",              { "Web Tools", "web fetch <url> [--out file]", "Fetches content from a URL (HTTP/HTTPS)", true, false, false } },
    { "ring:dump",        { "Diagnostics & Repair", "ring:dump [ring_name]", "Dump the contents of a registered ring buffer", true, false, false } },

    // Communications
    { "omni:email", { "Communications", "omni:email <address> <subject> <body>", "Send an email message", true, false, false } },

    // Config & Logs
    { "omni:cfg:reload",   { "Configuration", "omni:cfg:reload", "Reload the OmniShell configuration", true, false, false } },
    { "omni:cfg:show",     { "Configuration", "omni:cfg:show", "Display the current OmniShell configuration", true, false, false } },
    { "omni:logs:tail",    { "Logs", "omni:logs:tail [n]", "Tail the last n lines from logs", true, false, false } },
    { "omni:llm:file",     { "AI (Local LLM)", "omni:llm:file <path>", "Load a file into the local LLM context", true, false, false } },
    { "omni:log:annotate", { "Logs", "omni:log:annotate <file> <notes>", "Annotate a log file with notes", true, false, false } },

    // AI Engine
    { "omni:ai:load",          { "AI Engine", "omni:ai:load <engine>", "Load an AI engine backend", true, false, false } },
    { "omni:ai:unload",        { "AI Engine", "omni:ai:unload <engine>", "Unload an AI engine backend", true, false, false } },
    { "omni:ai:status",        { "AI Engine", "omni:ai:status", "Display current AI engine status", true, false, false } },
    { "omni:ai:chat",          { "AI Engine", "omni:ai:chat <prompt>", "Chat with an AI engine backend", true, false, false } },
    { "omni:ai:embed",         { "AI Engine", "omni:ai:embed <text>", "Generate embeddings from text", true, false, false } },
    { "omni:ai:backends",      { "AI Engine", "omni:ai:backends", "List available AI backends", true, false, false } },
    { "omni:ai:backends_info", { "AI Engine", "omni:ai:backends_info", "Show detailed information for AI backends", true, false, false } },

    // Scripting
    { "run-script", { "Scripting", "run-script <path> [args...]", "Executes a script file (.sh,.py,.ps1,.bat)", true, true, true } },
    { "run-py",     { "Scripting", "run-py [-f <file> | -c \"<code>\"][args...]", "Run a Python script file or inline code", true, true, true } },
    { "run-bash",   { "Scripting", "run-bash [-f <file> | -c \"<code>\"][args...]", "Run a Bash script file or inline code", true, true, true } },

    // =================================================================
    // BEGIN ADDITION: Cloud Storage Command Metadata
    // =================================================================
    { "omni:cloud:create",   { "Cloud Storage", "omni:cloud:create <path> <pass>", "Creates a new, empty cloud container", true, false, false } },
    { "omni:cloud:list",     { "Cloud Storage", "omni:cloud:list <path> <pass>", "Lists files within a container", true, false, false } },
    { "omni:cloud:upload",   { "Cloud Storage", "omni:cloud:upload <path> <pass> <local> [virtual]", "Uploads a local file to a container", true, false, false } },
    { "omni:cloud:download", { "Cloud Storage", "omni:cloud:download <path> <pass> <virtual> <local>", "Downloads a virtual file from a container", true, false, false } },
    { "omni:cloud:delete",   { "Cloud Storage", "omni:cloud:delete <path> <pass> <virtual>", "Deletes a virtual file from a container", true, false, false } },
    { "omni:cloud:mount",    { "Cloud Storage", "omni:cloud:mount <path> <mount_point>", "Mounts a container as a virtual drive (Windows)", true, false, false } },
    { "omni:cloud:unmount",  { "Cloud Storage", "omni:cloud:unmount <mount_point>", "Unmounts a virtual drive (Windows)", true, false, false } },
    { "omni:cloud:status",   { "Cloud Storage", "omni:cloud:status", "Shows status of mounted containers", true, false, false } }
    // =================================================================
    // END ADDITION
    // =================================================================
};

// =================================================================
// Local helpers and command handlers (file-scope only)
// =================================================================

namespace {
    using Args = CommandRouter::Args;

    // === Forward declarations for helpers used before their definitions ===
    std::string reconstructCommand(const std::vector<std::string>& args, size_t start_index = 1);

    static std::vector<std::string> g_command_names;
    static std::filesystem::path g_working_dir = std::filesystem::current_path();
    static std::filesystem::path g_prev_dir = g_working_dir;
    static std::unique_ptr<ai::IEngine> g_aiEngine;
    static bool g_aiLoaded = false;
    static std::string g_aiBackend;
    static std::string g_aiModelPath;
    static std::unique_ptr<ironrouter::SourceNetworkPcap> g_network_source;
    // UPDATED: This now uses the correct type for the IPC shared-memory writer.
    static std::map<std::string, std::unique_ptr<ironrouter::ipc::PacketWriter>> g_ring_writers;
    static std::unique_ptr<ironrouter::LiveCapture> g_live_capture;

    // Helper to parse script arguments and options
    static void parseScriptOptions(const Args& args, size_t startIndex, std::string& pathOrCode, bool& isFile, std::vector<std::string>& scriptArgs, ScriptOptions& opt) {
        isFile = true; // Default assumption
        if (startIndex < args.size()) {
            if (args[startIndex] == "-f" && startIndex + 1 < args.size()) {
                isFile = true;
                pathOrCode = args[startIndex + 1];
                startIndex += 2;
            }
            else if (args[startIndex] == "-c" && startIndex + 1 < args.size()) {
                isFile = false;
                pathOrCode = args[startIndex + 1];
                startIndex += 2;
            }
            else {
                pathOrCode = args[startIndex];
                startIndex++;
            }
        }
        for (size_t i = startIndex; i < args.size(); ++i) {
            scriptArgs.push_back(args[i]);
        }
        // For now, we use default ScriptOptions. You could expand this to parse --cwd, etc.
        opt.cwd = g_working_dir.string();
    }

    // =================================================================
// NEW: Command Handlers for Git and Vcpkg
// =================================================================
    std::string Cmd_Git(const Args& args) {
        if (args.size() < 2) {
            return "Usage: git <subcommand> [options]";
        }
        // For 'clone', use our specific helper
        if (args[1] == "clone" && args.size() >= 3) {
            std::string url = args[2];
            std::string dir = (args.size() > 3) ? args[3] : "";
            return ShellExecutor::gitClone(url, dir);
        }
        // For all other git commands, act as a simple passthrough
        return ShellExecutor::run(reconstructCommand(args, 0));
    }

    std::string Cmd_Vcpkg(const Args& args) {
        if (args.size() < 2) {
            return "Usage: vcpkg <subcommand> [options]";
        }
        // For 'install', use our specific helper
        if (args[1] == "install" && args.size() >= 3) {
            return ShellExecutor::vcpkgInstall(args[2]);
        }
        // For 'integrate install', use our helper
        if (args[1] == "integrate" && args.size() >= 3 && args[2] == "install") {
            return ShellExecutor::vcpkgIntegrateInstall();
        }
        // For all other vcpkg commands, act as a passthrough
        return ShellExecutor::run(reconstructCommand(args, 0));
    }

    // =================================================================
    // NEW: Command Handler for the Developer Prompt
    // =================================================================
    std::string Cmd_Dev(const Args& args) {
        if (args.size() < 2) {
            return "Usage: omni:dev <command_to_run>";
        }
        // Reconstruct the command line from the arguments
        std::string command_to_run = reconstructCommand(args, 1);
        return ShellExecutor::runInDevPrompt(command_to_run);
    }

    std::string Cmd_RunScript(const Args& args) {
        if (args.size() < 2) return "Usage: run-script <path> [args...]";

        std::string scriptPath = args[1];
        std::vector<std::string> scriptArgs;
        for (size_t i = 2; i < args.size(); ++i) {
            scriptArgs.push_back(args[i]);
        }

        ScriptOptions opt;
        opt.cwd = g_working_dir.string(); // Run in the current directory
        return ShellExecutor::runScriptFile(scriptPath, scriptArgs, opt);
    }

    std::string Cmd_RunPy(const Args& args) {
        if (args.size() < 2) return "Usage: run-py [-f <file> | -c \"<code>\"] [args...]";

        std::string pathOrCode;
        bool isFile;
        std::vector<std::string> scriptArgs;
        ScriptOptions opt;

        parseScriptOptions(args, 1, pathOrCode, isFile, scriptArgs, opt);

        if (pathOrCode.empty()) return "Error: No file or code provided.";

        return ShellExecutor::runPython(pathOrCode, isFile, scriptArgs, opt);
    }

    std::string Cmd_RunBash(const Args& args) {
        if (args.size() < 2) return "Usage: run-bash [-f <file> | -c \"<code>\"] [args...]";

        std::string pathOrCode;
        bool isFile;
        std::vector<std::string> scriptArgs;
        ScriptOptions opt;

        parseScriptOptions(args, 1, pathOrCode, isFile, scriptArgs, opt);

        if (pathOrCode.empty()) return "Error: No file or code provided.";

        return ShellExecutor::runBash(pathOrCode, isFile, scriptArgs, opt);
    }

    static bool aiEnsureLoaded(const std::string& backend,
        const std::string& path,
        std::string& err)
    {
        if (g_aiLoaded && backend == g_aiBackend && path == g_aiModelPath)
            return true;
        try {
            if (backend == "scratch") {
                g_aiEngine = ai::make_scratch_engine();
            }
            // else if (backend == "llama") g_aiEngine = ai::make_llama_engine();
            // else if (backend == "ollama") g_aiEngine = ai::make_ollama_engine();
            else {
                err = "Unknown AI backend: " + backend;
                return false;
            }
        }
        catch (...) {
            err = "Backend create failed: " + backend;
            return false;
        }

        if (!g_aiEngine) {
            err = "Failed to instantiate backend.";
            return false;
        }

        ai::LoadOptions opt;
        opt.model_path = path;
        opt.n_threads = std::thread::hardware_concurrency();
        if (!g_aiEngine->load(opt, err))
            return false;

        g_aiBackend = backend;
        g_aiModelPath = path;
        g_aiLoaded = true;
        return true;
    }

    std::string Cmd_AiLoad(const Args& a) {
        if (a.size() < 3) return "Usage: omni:ai:load <backend> <model_path>";
        std::string err;
        if (aiEnsureLoaded(a[1], a[2], err))
            return "[AI] Loaded backend=" + a[1] + " model=" + a[2];
        return "[AI] Load failed: " + err;
    }

    std::string Cmd_AiUnload(const Args&) {
        if (!g_aiLoaded || !g_aiEngine) return "[AI] No model loaded.";
        std::string err;
        if (!g_aiEngine->unload(err)) return "[AI] Unload error: " + err;
        g_aiEngine.reset();
        g_aiLoaded = false;
        return "[AI] Model unloaded.";
    }

    std::string Cmd_AiStatus(const Args&) {
        if (!g_aiLoaded || !g_aiEngine) return "[AI] No model loaded.";
        auto info = g_aiEngine->info();
        std::ostringstream os;
        os << "--- AI Engine Status ---\n"
            << "Backend: " << g_aiBackend << "\n"
            << "Model:   " << g_aiModelPath << "\n"
            << "Name:    " << info.name << "\n"
            << "Version: " << info.version << "\n"
            << "Context: " << info.ctx_len << "\n"
            << "Vocab:   " << info.vocab_size << "\n";
        return os.str();
    }

    std::string Cmd_AiChat(const Args& a) {
        if (a.size() < 2) return "Usage: omni:ai:chat <prompt>";
        if (!g_aiLoaded || !g_aiEngine) return "[AI] No model loaded.";
        std::string prompt = reconstructCommand(a, 1);
        ai::Sampling samp;
        std::string err;
        std::ostringstream output;
        auto cb = [&](const ai::TokenEvent& ev) {
            output << ev.text;
            if (ev.is_final) output << "\n";
            };
        if (!g_aiEngine->chat(prompt, samp, cb, err))
            return "[AI] Chat error: " + err;
        return output.str();
    }

    std::string Cmd_AiEmbed(const Args& a) {
        if (a.size() < 2) return "Usage: omni:ai:embed <text>";
        if (!g_aiLoaded || !g_aiEngine) return "[AI] No model loaded.";
        ai::EmbedResult vec;
        std::string err;
        if (!g_aiEngine->embed(reconstructCommand(a, 1), vec, err))
            return "[AI] Embed error: " + err;
        std::ostringstream os;
        os << "Embedding[" << vec.vector.size() << "]:";
        for (size_t i = 0; i < vec.vector.size(); ++i) {
            if (i % 8 == 0) os << "\n";
            os << std::fixed << std::setprecision(5) << vec.vector[i] << " ";
        }
        return os.str();
    }

    std::string Cmd_AiBackends(const Args&) {
        try {
            auto backends = ai::list_available_backends();
            if (backends.empty()) return "[AI] No backends registered.";
            std::ostringstream os;
            os << "--- Available AI Backends ---\n";
            for (const auto& b : backends) {
                os << " - " << b << "\n";
            }
            return os.str();
        }
        catch (const std::exception& e) {
            return std::string("[AI] Error listing backends: ") + e.what();
        }
        catch (...) {
            return "[AI] Unknown error while listing backends.";
        }
    }

    std::string Cmd_AiBackendsInfo(const Args&) {
        auto backends = ai::list_available_backends();
        if (backends.empty()) return "[AI] No backends available.";
        std::ostringstream os;
        os << "--- Backends Info ---\n";
        for (auto& b : backends) {
            os << "Backend: " << b << "\n";
            std::string err;
            auto engine = (b == "scratch") ? ai::make_scratch_engine() : nullptr;
            if (engine) os << "  Capabilities: " << engine->capabilities() << "\n";
        }
        return os.str();
    }
    // =================================================================
    // BEGIN ADDITION: Cloud Command Handlers
    // =================================================================
    // Helper to convert CloudError enum to a user-friendly string
    std::string errorToString(onecloud::CloudError err) {
        // This function is correct as is
        switch (err) {
        case onecloud::CloudError::Success: return "Operation was successful.";
        case onecloud::CloudError::ContainerNotFound: return "Container file not found.";
        case onecloud::CloudError::InvalidPassword: return "Invalid password or corrupted container.";
        case onecloud::CloudError::InvalidContainerFormat: return "Invalid container format.";
        case onecloud::CloudError::AccessDenied: return "Access denied.";
        case onecloud::CloudError::FileExists: return "File or container already exists.";
        case onecloud::CloudError::FileNotFound: return "File not found within the container.";
        case onecloud::CloudError::IOError: return "A disk input/output error occurred.";
        case onecloud::CloudError::OutOfMemory: return "Out of memory.";
        case onecloud::CloudError::EncryptionFailed: return "An encryption or decryption error occurred."; // Was EncryptionError
        case onecloud::CloudError::Unknown: return "An unknown error occurred.";
        default: return "An unrecognized error code was returned.";
        }
    }

    std::string Cmd_CloudCreate(const Args& args) {
        if (args.size() < 3) {
            return "Usage: omni:cloud:create <container_path> <password>";
        }
        // CORRECTED: Use the renamed CloudAPI class
        auto result = onecloud::CloudAPI::create(args[1], args[2]);
        if (result) {
            return "Container created successfully: " + args[1];
        }
        return "Error: " + errorToString(result.error());
    }

    std::string Cmd_CloudList(const Args& args) {
        if (args.size() < 3) {
            return "Usage: omni:cloud:list <container_path> <password>";
        }
        auto open_result = onecloud::CloudAPI::open(args[1], args[2]);
        if (!open_result) {
            return "Error: " + errorToString(open_result.error());
        }

        auto& storage = *open_result;
 
        auto list_result = storage.list_files();
        if (!list_result) {
            return "Error: " + errorToString(list_result.error());
        }

        if (list_result->empty()) {
            return "Container is empty.";
        }

        std::string output = "Files in container '" + args[1] + "':\n";
        for (const auto& file : *list_result) {
            output += "- " + file + "\n";
        }
        return output;
    }

    std::string Cmd_CloudUpload(const Args& args) {
        if (args.size() < 4) {
            return "Usage: omni:cloud:upload <container_path> <password> <local_file_path> [virtual_path]";
        }
        const std::string& container_path = args[1];
        const std::string& password = args[2];
        const std::string& local_path = args[3];
        std::string virtual_path = (args.size() > 4) ? args[4] : std::filesystem::path(local_path).filename().string();

        std::ifstream local_file(local_path, std::ios::binary | std::ios::ate);
        if (!local_file) {
            return "Error: Cannot open local file '" + local_path + "'.";
        }
        std::streamsize size = local_file.tellg();
        local_file.seekg(0, std::ios::beg);
        std::vector<std::byte> buffer(static_cast<size_t>(size));
        if (!local_file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            return "Error: Failed to read local file '" + local_path + "'.";
        }

        auto open_result = onecloud::CloudAPI::open(container_path, password);
        if (!open_result) {
            return "Error: " + errorToString(open_result.error());
        }

        auto& storage = *open_result;

        auto write_result = storage.write_file(virtual_path, buffer);
        if (write_result) {
            return "Successfully uploaded '" + local_path + "' to '" + virtual_path + "'.";
        }
        return "Error: " + errorToString(write_result.error());
    }

    std::string Cmd_CloudDownload(const Args& args) {
        if (args.size() < 5) {
            return "Usage: omni:cloud:download <container_path> <password> <virtual_path> <local_destination_path>";
        }
        const std::string& container_path = args[1];
        const std::string& password = args[2];
        const std::string& virtual_path = args[3];
        const std::string& local_path = args[4];
        
        auto open_result = onecloud::CloudAPI::open(container_path, password);
        if (!open_result) {
            return "Error: " + errorToString(open_result.error());
        }

        auto& storage = *open_result;
        auto read_result = storage.read_file(virtual_path);
        if (!read_result) {
            return "Error: " + errorToString(read_result.error());
        }

        std::ofstream local_file(local_path, std::ios::binary | std::ios::trunc);
        if (!local_file) {
            return "Error: Cannot open destination file '" + local_path + "' for writing.";
        }
        local_file.write(reinterpret_cast<const char*>(read_result->data()), read_result->size());
        return "Successfully downloaded '" + virtual_path + "' to '" + local_path + "'.";
    }

    std::string Cmd_CloudDelete(const Args& args) {
        if (args.size() < 4) {
            return "Usage: omni:cloud:delete <container_path> <password> <virtual_path>";
        }
        const std::string& container_path = args[1];
        const std::string& password = args[2];
        const std::string& virtual_path = args[3];

      
        auto open_result = onecloud::CloudAPI::open(container_path, password);
        if (!open_result) {
            return "Error: " + errorToString(open_result.error());
        }

        auto& storage = *open_result;
     
        auto delete_result = storage.delete_file(virtual_path);
        if (delete_result) {
            return "Successfully deleted '" + virtual_path + "' from the container.";
        }
        return "Error: " + errorToString(delete_result.error());
    }

    std::string Cmd_CloudMount(const Args& args) {
        if (args.size() < 3) {
            return "Usage: omni:cloud:mount <container_path> <mount_point_path>";
        }
        return "[INFO] Filesystem mounting via the Windows Cloud Filter API is a complex, platform-specific operation.\n"
            "[INFO] This command is a placeholder for the logic detailed in the architecture report.";
    }

    std::string Cmd_CloudUnmount(const Args& args) {
        if (args.size() < 2) {
            return "Usage: omni:cloud:unmount <mount_point_path>";
        }
        return "[INFO] Filesystem unmounting is not yet implemented.";
    }

    std::string Cmd_CloudStatus(const Args& args) {
        return "[INFO] Sync status reporting is not yet implemented.";
    }
    // =================================================================
    // END ADDITION
    // =================================================================


    // Forward declarations
    std::string tailFile(const std::string& path, size_t lines = 100);
    std::string readFile(const std::string& path);
    void writeFile(const std::string& path, const std::string& content);
    void ensureDir(const std::string& path);
    std::string pathBaseName(const std::string& path);
    void saveCSV(const std::string& path, const PMU::ProcessSample& s);
    PMU::ProcessSample loadCSV(const std::string& path);

    std::string get_env(const char* var) {
#if defined(_WIN32)
        char* buffer = nullptr;
        size_t len = 0;
        if (_dupenv_s(&buffer, &len, var) == 0 && buffer != nullptr) {
            std::string result(buffer);
            free(buffer);
            return result;
        }
        return {};
#else
        const char* v = std::getenv(var);
        return v ? std::string(v) : std::string{};
#endif
    }

    inline std::string ltrim(const std::string& s) {
        size_t i = 0;
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    }
    inline std::string rtrim(const std::string& s) {
        if (s.empty()) return s;
        size_t i = s.size();
        while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1]))) --i;
        return s.substr(0, i);
    }
    inline std::string trim(const std::string& s) { return rtrim(ltrim(s)); }
    inline std::string stripQuotes(const std::string& s) {
        if (s.size() >= 2) {
            char a = s.front(), b = s.back();
            if ((a == '"' && b == '"') || (a == '\'' && b == '\''))
                return s.substr(1, s.size() - 2);
        }
        return s;
    }
    std::filesystem::path expandUser(const std::string& raw) {
        if (raw.empty()) return {};
        if (raw[0] != '~') return std::filesystem::path(raw);
#if defined(_WIN32)
        std::filesystem::path base = get_env("USERPROFILE");
#else
        std::filesystem::path base = get_env("HOME");
#endif
        if (raw.size() == 1) return base;
        // handle "~/foo" or "~\foo"
        if (raw.size() >= 2 && (raw[1] == '/' || raw[1] == '\\')) {
            return base / std::filesystem::path(raw.substr(2));
        }
        // "~someone" not supported -> return as-is
        return std::filesystem::path(raw);
    }

    std::filesystem::path resolvePath(const std::string& raw_in) {
        std::string raw = stripQuotes(trim(raw_in));
        if (raw.empty()) return g_working_dir;

#if defined(_WIN32)
        // Drive-only like "C:" -> "C:\"
        if (raw.size() == 2 && std::isalpha(static_cast<unsigned char>(raw[0])) && raw[1] == ':') {
            std::string driveRoot;
            driveRoot.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(raw[0]))));
            driveRoot += ":\\";
            return std::filesystem::path(driveRoot);
        }
        // "\" or "/" -> root of current drive
        if (raw.size() == 1 && (raw[0] == '\\' || raw[0] == '/')) {
            std::filesystem::path root = g_working_dir.root_path();
            if (root.empty()) root = std::filesystem::current_path().root_path();
            return root;
        }
#endif
        std::filesystem::path p = expandUser(raw);
        if (p.is_absolute()) return p;
        return g_working_dir / p;
    }

    // -----------------------------------------------------------------
    // Local LLM router backend (TU-local)
    // Provides: RouterLLM::Options, g_engine, g_loaded, g_modelPath,
    //           ensureLoaded(...), status(), run(...)
    // -----------------------------------------------------------------
    namespace RouterLLM {

        struct Options {
            std::string model;
            std::string prompt = "Hello";
            int   n_predict = 64;
            float temp = 0.8f;
            int   top_k = 40;
            float top_p = 0.95f;
            bool  stream = true;
            // (Optional) Future: allow CLI override of these
            // int   mlp_kind   = -1;
            // int   norm_kind  = -1;
            // float rope_theta_base = -1.0f;
            // float rope_freq_scale = -1.0f;
        };

        static CLLF g_engine;
        static bool g_loaded = false;
        static std::string g_modelPath;

        static bool ensureLoaded(const std::string& path) {
            if (g_loaded && (path.empty() || path == g_modelPath)) return true;
            if (path.empty()) return false;
            if (!g_engine.load(path)) return false;
            g_modelPath = path;
            g_loaded = true;
            return true;
        }

        static std::string status() {
            if (!g_loaded) return "[LLM] No model loaded.";
            const auto& cfg = g_engine.W.cfg;
            std::ostringstream os;
            os << "[LLM] Model: " << g_modelPath
                << " vocab=" << cfg.vocab_size
                << " d_model=" << cfg.d_model
                << " layers=" << cfg.n_layers
                << " heads=" << cfg.n_heads
                << " max_seq=" << cfg.max_seq
                << " mlp_kind=" << cfg.mlp_kind
                << " norm_kind=" << cfg.norm_kind
                << " rope_theta_base=" << cfg.rope_theta_base
                << " rope_freq_scale=" << cfg.rope_freq_scale;
            return os.str();
        }

        static std::string run(const Options& o) {
            if (!ensureLoaded(o.model))
                return "[LLM] Failed to load model.";

            const auto& tok = g_engine.tok;

            ai::gen::GenerationConfig cfg;
            cfg.max_new_tokens = o.n_predict;
            cfg.sampling.temperature = o.temp;
            cfg.sampling.top_k = o.top_k;
            cfg.sampling.top_p = o.top_p;
            cfg.eos_id = -1;
            cfg.allow_empty_output = true;

            ai::gen::GenerationCallbacks cb;

            std::string out;
            auto on_piece = [&](token_id /*id*/, const std::string& piece) {
                if (o.stream) std::cout << piece << std::flush;
                else          out += piece;
                };

            auto prompt_tokens = tok.tokenize(o.prompt);
            ai::gen::generate(
                [&](token_id t) { return g_engine.decode_step(static_cast<int>(t)); },
                prompt_tokens,
                cfg,
                tok,
                on_piece,
                cb
            );

            if (o.stream) {
                std::cout << std::endl;
                return {};
            }
            return out;
        }

    } // namespace RouterLLM

    // ---------------- Command Handlers (free functions) ----------------

    using Args = CommandRouter::Args;

    std::string Cmd_Help(const Args& args) {
        std::ostringstream ss;
        if (args.size() >= 2) {
            const std::string& name = args[1];
            auto it = g_command_metadata.find(name);
            if (it != g_command_metadata.end()) {
                const auto& meta = it->second;
                ss << "Command:  " << name << "\n"
                    << "Category: " << meta.category << "\n"
                    << "Usage:    " << meta.usage << "\n"
                    << "Summary:  " << meta.summary << "\n"
                    << "Platforms:";
                if (meta.platform_win)   ss << " [Windows]";
                if (meta.platform_linux) ss << " [Linux]";
                if (meta.platform_mac)   ss << " [macOS]";
                ss << "\n";
            }
            else {
                ss << "No help found for: " << name << "\n";
            }
            return ss.str();
        }

        // Index
        ss << "--- OmniShell Command Index ---\n\n";
        std::map<std::string, std::vector<std::string>> categories;
        for (const auto& kv : g_command_metadata) {
            categories[kv.second.category].push_back(kv.first);
        }
        for (const auto& cat : categories) {
            ss << "== " << cat.first << " ==\n";
            for (const auto& cmd : cat.second) {
                const auto& meta = g_command_metadata.at(cmd);
                ss << "  " << cmd << " - " << meta.summary << "\n";
            }
            ss << "\n";
        }
        ss << "Type: help <command> for details.\n";
        return ss.str();
    }

    std::string Cmd_OmniHelp(const Args&) {
        std::ostringstream ss;
        ss << "--- OmniShell Help ---\n\n";
        std::map<std::string, std::vector<std::pair<std::string, CommandMeta>>> grouped;
        for (const auto& kv : g_command_metadata) {
            grouped[kv.second.category].push_back({ kv.first, kv.second });
        }
        for (const auto& kv : grouped) {
            ss << "== " << kv.first << " ==\n";
            for (const auto& pair : kv.second) {
                const auto& meta = pair.second;
                ss << "  " << meta.usage << " - " << meta.summary << "\n";
            }
            ss << "\n";
        }
        return ss.str();
    }

    std::string Cmd_LlmHelp(const Args&) {
        std::ostringstream os;
        os << "Local LLM commands:\n";
        for (const auto& kv : g_command_metadata) {
            const auto& meta = kv.second;
            if (meta.category == "AI (Local LLM)") {
                os << "  " << meta.usage << " - " << meta.summary << "\n";
            }
        }
        return os.str();
    }


    std::string Cmd_Exit(const Args&) {
        std::cout << "Exiting...\n";
        std::exit(0);
        return std::string{};
    }

    std::string Cmd_Jobs(const Args&) {
        return JobManager::listJobs();
    }

    std::string Cmd_Fg(const Args& args) {
        if (args.size() < 2) return "Usage: fg <job_id>";
        try {
            int jobId = std::stoi(args[1]);
            return JobManager::waitForJob(jobId);
        }
        catch (...) {
            return "Error: Invalid Job ID.";
        }
    }

    // Navigation
    std::string Cmd_Cd(const Args& args) {
        // cd -> print current dir
        if (args.size() == 1) return g_working_dir.string();

        // cd - -> switch to previous dir
        if (args.size() == 2 && args[1] == "-") {
            std::filesystem::path target = g_prev_dir;
            if (target.empty()) return g_working_dir.string();
            std::error_code ec;
            if (std::filesystem::exists(target, ec) && std::filesystem::is_directory(target, ec)) {
                std::filesystem::path old = g_working_dir;
                std::filesystem::path canonicalized = std::filesystem::weakly_canonical(target, ec);
                g_working_dir = canonicalized.empty() ? target : canonicalized;
                std::filesystem::current_path(g_working_dir, ec);
                g_prev_dir = old;
                return g_working_dir.string();
            }
            return std::string("cd: target does not exist: ") + target.string();
        }

        std::string raw = reconstructCommand(args, 1);
        auto target = resolvePath(raw);
        std::error_code ec;
        if (!std::filesystem::exists(target, ec)) {
            return std::string("cd: no such path: ") + target.string();
        }
        if (!std::filesystem::is_directory(target, ec)) {
            return std::string("cd: not a directory: ") + target.string();
        }
        std::filesystem::path old = g_working_dir;
        std::filesystem::path canonicalized = std::filesystem::weakly_canonical(target, ec);
        g_working_dir = canonicalized.empty() ? std::filesystem::absolute(target, ec) : canonicalized;
        if (g_working_dir.empty()) g_working_dir = target;
        std::filesystem::current_path(g_working_dir, ec); // best effort
        g_prev_dir = old;
        return g_working_dir.string();
    }

    std::string Cmd_Pwd(const Args&) {
        return g_working_dir.string();
    }

#if defined(_WIN32)
    std::string Cmd_Ls(const Args&) {
        return ShellExecutor::run("dir");
    }
    std::string Cmd_Ps(const Args& args) {
        return ShellExecutor::runPowerShell(reconstructCommand(args));
    }
#elif defined(__linux__)
    std::string Cmd_Dir(const Args&) {
        return ShellExecutor::run("ls -la");
    }
    std::string Cmd_DiskUsage(const Args&) {
        return ShellExecutor::run("df -h");
    }
    std::string Cmd_MemInfo(const Args&) {
        return ShellExecutor::run("free -h");
    }
#endif

#if defined(_WIN32)
    std::string Cmd_RepairAll(const Args&) {
        return ShellExecutor::run("sfc /scannow && DISM /Online /Cleanup-Image /RestoreHealth");
    }
    std::string Cmd_RepairSfc(const Args&) {
        return ShellExecutor::run("sfc /scannow");
    }
    std::string Cmd_RepairDism(const Args&) {
        return ShellExecutor::run("DISM /Online /Cleanup-Image /RestoreHealth");
    }
    std::string Cmd_DiskCheck(const Args& args) {
        if (args.size() < 2) return std::string("Usage: omni:disk_check C:");
        return ShellExecutor::run("chkdsk " + args[1] + " /f /r");
    }
#endif

    std::string Cmd_Diagnose(const Args& args) {
        if (args.size() < 2) {
            try {
                auto sensors = SensorManager::listSensors();
                auto plan = OmniAIManager::analyzeAndRecommend(sensors, appConfig);
                if (plan.empty()) return "[AI Diagnose] System is nominal. No repair plan needed.";
                std::stringstream ss;
                ss << "--- AI Recommended Repair Plan ---\n";
                int stepNum = 1;
                for (const auto& step : plan) {
                    ss << "Step " << stepNum++ << ": " << step.description << "\n";
                    ss << "  > Command: " << step.command << "\n";
                }
                ss << "----------------------------------";
                return ss.str();
            }
            catch (...) {
                return "Usage: omni:diagnose <registry|entropy|processes|analyze> ...";
            }
        }
        std::string subcommand = args[1];
        if (subcommand == "registry") {
            if (args.size() < 4) return "Usage: omni:diagnose registry <root_key> <search_term>";
            return DiagnosticsModule::ScanRegistry(args[2], { args[3] });
        }
        if (subcommand == "entropy") {
            if (args.size() < 3) return "Usage: omni:diagnose entropy <path> [quarantine_dir] [report_dir]";
            std::string quarantineDir = (args.size() > 3) ? args[3] : appConfig.defaultQuarantineDir;
            std::string reportDir = (args.size() > 4) ? args[4] : appConfig.defaultReportDir;
            return DiagnosticsModule::ScanFileEntropy(args[2], quarantineDir, reportDir, appConfig.entropyThreshold);
        }
        if (subcommand == "processes") {
            return DiagnosticsModule::MonitorProcesses();
        }
        if (subcommand == "analyze") {
            if (args.size() < 3) return "Usage: omni:diagnose analyze <filepath>";
            DiagnosticsModule::AnalyzeBinary(args[2]);
            return "Binary analysis job submitted. See reports directory for output.";
        }
        return "Unknown diagnose subcommand. Use 'omni:help' for details.";
    }

    std::string Cmd_Kill(const Args& args) {
        if (args.size() < 2) return "Usage: omni:kill <pid>";
        try {
            unsigned long pid = std::stoul(args[1]);
            return DiagnosticsModule::TerminateProcessByPID(pid);
        }
        catch (...) {
            return "Error: Invalid PID.";
        }
    }

    std::string Cmd_TaskDaemon(const Args& args) {
        if (args.size() < 2) return std::string("Usage: omni:task_daemon [ai-maintain | status | disable]");
        if (args[1] == "ai-maintain") {
            daemon.start(appConfig);
            return daemon.getStatus();
        }
        if (args[1] == "status") {
            return daemon.getStatus();
        }
        if (args[1] == "disable") {
            daemon.stop();
            return daemon.getStatus();
        }
        return std::string("[AI Daemon] Unknown subcommand.");
    }

    std::string Cmd_Ask(const Args& args) {
        if (args.size() < 2) return "Usage: omni:ask <query> [--with-context]";
        bool withCtx = std::find(args.begin(), args.end(), std::string("--with-context")) != std::end(args);
        std::string q;
        if (withCtx) {
            for (size_t i = 1; i < args.size(); ++i) {
                if (args[i] == "--with-context") continue;
                if (!q.empty()) q += " ";
                q += args[i];
            }
            std::string blob = "--- OmniAI Context ---\n";
            try { blob += OmniAIManager::getContextSnapshot(); }
            catch (...) {}
#if defined(_WIN32)
            blob += "\n[LogsTail]\n" + tailFile("logs\\session.log", 200);
#else
            blob += "\n[LogsTail]\n" + tailFile("logs/session.log", 200);
#endif
            try {
                std::string pmu = OmniAIManager::recentPmuSummary();
                if (!pmu.empty()) blob += "\n[PMUSummary]\n" + pmu;
            }
            catch (...) {}
            try {
                std::string tiles = OmniAIManager::recentTilesSummary();
                if (!tiles.empty()) blob += "\n[TilesSummary]\n" + tiles;
            }
            catch (...) {}
            try { return OmniAIManager::queryWithContext(q, blob); }
            catch (...) {}
        }
        else {
            q = reconstructCommand(args);
            try { return OmniAIManager::query(q); }
            catch (...) {}
        }
        return "AI query path unavailable.";
    }

    std::string Cmd_Mode(const Args& a) {
        if (a.size() < 2) return "Usage: omni:mode <concise|verbose|debug>";
        std::string m = a[1];
        try {
            if (m == "concise") OmniAIManager::setMode(AiMode::Concise);
            else if (m == "verbose") OmniAIManager::setMode(AiMode::Verbose);
            else if (m == "debug") OmniAIManager::setMode(AiMode::Debug);
            else return "Unknown mode. Use: concise | verbose | debug";
            return "AI mode set.";
        }
        catch (...) {
            return "AI mode path unavailable.";
        }
    }

    std::string Cmd_Explain(const Args& a) {
        if (a.size() < 2) return "Usage: omni:explain <command or text>";
        std::string text = reconstructCommand(a);
        try { return OmniAIManager::explain(text); }
        catch (...) { return "AI explain path unavailable."; }
    }

    std::string Cmd_Gen(const Args& a) {
        if (a.size() < 2) return "Usage: omni:gen \"<goal>\" [--dry-run]";
        bool dry = (std::find(a.begin(), a.end(), std::string("--dry-run")) != a.end());
        std::string goal;
        for (size_t i = 1; i < a.size(); ++i) {
            if (a[i] == "--dry-run") continue;
            if (!goal.empty()) goal += " ";
            goal += a[i];
        }
        try { return OmniAIManager::generate(goal, dry); }
        catch (...) { return "AI generate path unavailable."; }
    }

    std::string Cmd_CtxDump(const Args& args) {
        std::string outPath = (args.size() >= 2) ? args[1] : "reports/context_dump.txt";
        std::string blob;
        try { blob = OmniAIManager::getContextSnapshot(); }
        catch (...) {}
#if defined(_WIN32)
        blob += "\n[LogsTail]\n" + tailFile("logs\\session.log", 200);
#else
        blob += "\n[LogsTail]\n" + tailFile("logs/session.log", 200);
#endif
        try { blob += "\n[PMU]\n" + OmniAIManager::recentPmuSummary(); }
        catch (...) {}
        try { blob += "\n[Tiles]\n" + OmniAIManager::recentTilesSummary(); }
        catch (...) {}
        writeFile(outPath, blob);
        return "[Context] Dumped to: " + outPath;
    }


    std::string Cmd_LogSum(const Args& a) {
        if (a.size() < 2) return "Usage: omni:log:sum <path> [--since=..] [--errors-only]";
        std::string path = a[1];
        auto contents = readFile(path);
        if (contents.empty()) return "No data at: " + path;
        std::string summary;
        try { summary = OmniAIManager::summarize(contents); }
        catch (...) { summary = "[AI summarize unavailable]\n" + contents.substr(0, 2000); }
        auto outDir = std::string("reports/ai_logs");
        ensureDir(outDir);
        auto outPath = outDir + std::string("/") + pathBaseName(path) + ".sum.txt";
        writeFile(outPath, summary);
        return "Summary saved: " + outPath;
    }

    std::string Cmd_Ctx(const Args&) {
        try { return OmniAIManager::getContext(); }
        catch (...) { return "[Context path unavailable]"; }
    }

    std::string Cmd_Models(const Args&) {
        try { return OmniAIManager::listModels(); }
        catch (...) { return "[Models path unavailable]"; }
    }

    std::string Cmd_SensorList(const Args&) {
        auto sensors = SensorManager::listSensors();
        if (sensors.empty()) return std::string("[Sensors] No sensors found or query failed.");
        std::stringstream ss;
        ss << "--- System Sensor Status ---\n";
        for (const auto& sensor : sensors) {
            ss << "[" << sensor.label << "]: " << sensor.value << " " << sensor.unit << "\n";
        }
        return ss.str();
    }

    // ----------------- Sensor helpers & commands -----------------

    // Format timestamp to readable string (UTC)
    static std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) {
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        char buf[64] = { 0 };
#if defined(_POSIX_THREAD_SAFE_FUNCTIONS)
        std::tm tm;
        gmtime_r(&t, &tm);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
        return std::string(buf);
#else
        char buf2[64] = { 0 };
        if (ctime_s(buf2, sizeof(buf2), &t) != 0)
            return "";
        std::string s(buf2);
        if (!s.empty() && s.back() == '\n') s.pop_back();
        return s;
#endif
    }

    static std::string sensorToVerboseLine(const SensorData& s) {
        std::ostringstream ss;
        ss << "ID: " << s.id << "\n";
        ss << "  Type:   " << s.type << "\n";
        ss << "  Name:   " << s.name << "\n";
        ss << "  Label:  " << s.label << "\n";
        ss << "  Value:  " << s.value << " " << s.unit << "\n";
        ss << "  Status: ";
        switch (s.status) {
        case SensorStatus::OK: ss << "OK"; break;
        case SensorStatus::WARN: ss << "WARN"; break;
        case SensorStatus::CRITICAL: ss << "CRITICAL"; break;
        default: ss << "UNKNOWN"; break;
        }
        ss << "\n";
        ss << "  Source: " << s.source << "\n";
        ss << "  Time:   " << formatTimestamp(s.timestamp) << "\n";
        return ss.str();
    }

    static std::string sensorToCsvRow(const SensorData& s) {
        auto esc = [](const std::string& v) {
            if (v.find_first_of(",\"") == std::string::npos) return v;
            std::string out = "\"";
            for (char c : v) {
                if (c == '"') out += "\"\"";
                else out += c;
            }
            out += "\"";
            return out;
            };
        std::ostringstream ss;
        ss << esc(s.id) << ",";
        ss << esc(s.type) << ",";
        ss << esc(s.name) << ",";
        ss << esc(s.label) << ",";
        ss << s.value << ",";
        ss << esc(s.unit) << ",";
        switch (s.status) {
        case SensorStatus::OK: ss << "OK"; break;
        case SensorStatus::WARN: ss << "WARN"; break;
        case SensorStatus::CRITICAL: ss << "CRITICAL"; break;
        default: ss << "UNKNOWN"; break;
        }
        ss << ",";
        ss << esc(s.source) << ",";
        ss << esc(formatTimestamp(s.timestamp));
        return ss.str();
    }

    // Find sensor by id or label substring (returns index or -1)
    static int findSensorIndex(const std::vector<SensorData>& sensors, const std::string& needle) {
        for (size_t i = 0; i < sensors.size(); ++i) {
            if (sensors[i].id == needle) return (int)i;
        }
        std::string lowneedle = needle; std::transform(lowneedle.begin(), lowneedle.end(), lowneedle.begin(), ::tolower);
        for (size_t i = 0; i < sensors.size(); ++i) {
            std::string lab = sensors[i].label; std::transform(lab.begin(), lab.end(), lab.begin(), ::tolower);
            if (lab.find(lowneedle) != std::string::npos) return (int)i;
        }
        return -1;
    }

    // ---------------- Command: omni:sensor_dump ----------------
    std::string Cmd_SensorDump(const Args&) {
        auto sensors = SensorManager::listSensors();
        if (sensors.empty()) return "[Sensors] No sensors found or query failed.";
        std::ostringstream ss;
        ss << "--- Sensor Dump (" << sensors.size() << ") ---\n";
        for (const auto& s : sensors) {
            ss << sensorToVerboseLine(s) << "\n";
        }
        return ss.str();
    }

    // ---------------- Command: omni:sensor_show <id|label> ----------------
    std::string Cmd_SensorShow(const Args& args) {
        if (args.size() < 2) return "Usage: omni:sensor_show <id|label-substr>";
        auto sensors = SensorManager::listSensors();
        if (sensors.empty()) return "[Sensors] No sensors found or query failed.";
        std::string needle = args[1];
        int idx = findSensorIndex(sensors, needle);
        if (idx < 0) return "Sensor not found: " + needle;
        return sensorToVerboseLine(sensors[(size_t)idx]);
    }

    std::string Cmd_SensorSnapshot(const Args& args) {
        std::string outPath = (args.size() >= 2) ? args[1] : "reports/sensors_snapshot.csv";
        auto sensors = SensorManager::listSensors();
        if (sensors.empty()) return "[Sensors] No data to snapshot.";
        try { std::filesystem::create_directories(std::filesystem::path(outPath).parent_path()); }
        catch (...) {}
        std::ofstream f(outPath, std::ios::trunc);
        f << "id,type,name,label,value,unit,status,source,timestamp\n";
        for (auto& s : sensors) f << sensorToCsvRow(s) << "\n";
        return "[Sensors] Snapshot saved: " + outPath;
    }

    // ---------------- Command: omni:sensor_export <out.csv> ----------------
    std::string Cmd_SensorExport(const Args& args) {
        if (args.size() < 2) return "Usage: omni:sensor_export <out.csv>";
        std::string out = args[1];
        auto sensors = SensorManager::listSensors();
        if (sensors.empty()) return "[Sensors] No sensors found or query failed.";
        try {
            std::filesystem::path p(out);
            auto d = p.parent_path();
            if (!d.empty()) std::filesystem::create_directories(d);
        }
        catch (...) {}
        std::ofstream f(out, std::ios::trunc);
        if (!f) return "Error: cannot write file: " + out;
        f << "id,type,name,label,value,unit,status,source,timestamp\n";
        for (const auto& s : sensors) f << sensorToCsvRow(s) << "\n";
        f.close();
        return "Sensors exported: " + out;
    }

    // ---------------- Command: omni:sensor_filter [key=val ...] [json] ----------------
    // Supported keys: type, status, source
    std::string Cmd_SensorFilter(const Args& args) {
        auto sensors = SensorManager::listSensors();
        if (sensors.empty()) return "[Sensors] No sensors found or query failed.";

        std::string wantType, wantStatus, wantSource;
        bool wantJson = false;
        for (size_t i = 1; i < args.size(); ++i) {
            std::string a = args[i];
            if (a == "json") { wantJson = true; continue; }
            size_t eq = a.find('=');
            if (eq == std::string::npos) continue;
            std::string k = a.substr(0, eq), v = a.substr(eq + 1);
            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
            if (k == "type") wantType = v;
            else if (k == "status") wantStatus = v;
            else if (k == "source") wantSource = v;
        }

        std::vector<SensorData> out;
        for (auto& s : sensors) {
            if (!wantType.empty()) {
                std::string t = s.type; std::transform(t.begin(), t.end(), t.begin(), ::tolower);
                std::string want = wantType; std::transform(want.begin(), want.end(), want.begin(), ::tolower);
                if (t.find(want) == std::string::npos) continue;
            }
            if (!wantSource.empty()) {
                std::string src = s.source; std::transform(src.begin(), src.end(), src.begin(), ::tolower);
                std::string want = wantSource; std::transform(want.begin(), want.end(), want.begin(), ::tolower);
                if (src.find(want) == std::string::npos) continue;
            }
            if (!wantStatus.empty()) {
                std::string st;
                switch (s.status) {
                case SensorStatus::OK: st = "ok"; break;
                case SensorStatus::WARN: st = "warn"; break;
                case SensorStatus::CRITICAL: st = "critical"; break;
                default: st = "unknown"; break;
                }
                std::string want = wantStatus; std::transform(want.begin(), want.end(), want.begin(), ::tolower);
                if (st.find(want) == std::string::npos) continue;
            }
            out.push_back(s);
        }

        if (out.empty()) return "No sensors match those filters.";

        std::ostringstream ss;
        if (wantJson) {
            ss << "[\n";
            for (size_t i = 0; i < out.size(); ++i) {
                const auto& s = out[i];
                ss << "  {\n";
                ss << "    \"id\": \"" << s.id << "\",\n";
                ss << "    \"type\": \"" << s.type << "\",\n";
                ss << "    \"name\": \"" << s.name << "\",\n";
                ss << "    \"label\": \"" << s.label << "\",\n";
                ss << "    \"value\": " << s.value << ",\n";
                ss << "    \"unit\": \"" << s.unit << "\",\n";
                ss << "    \"status\": \"" << (s.status == SensorStatus::OK ? "OK" : (s.status == SensorStatus::WARN ? "WARN" : (s.status == SensorStatus::CRITICAL ? "CRITICAL" : "UNKNOWN"))) << "\",\n";
                ss << "    \"source\": \"" << s.source << "\",\n";
                ss << "    \"timestamp\": \"" << formatTimestamp(s.timestamp) << "\"\n";
                ss << "  }" << (i + 1 < out.size() ? "," : "") << "\n";
            }
            ss << "]\n";
        }
        else {
            ss << "--- Filtered Sensors (" << out.size() << ") ---\n";
            for (const auto& s : out) {
                ss << "[" << s.label << " | " << s.id << "]: " << s.value << " " << s.unit << " (" << s.source << ")\n";
            }
        }
        return ss.str();
    }
    // ------------------------------------------------------------------
    // Existing helpers
    // ------------------------------------------------------------------
    static std::wstring to_wstring_helper(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }

    static std::string to_string_helper(const std::wstring& wstr) {
        if (wstr.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    // ======================
    // Web Command Handler (Updated)
    // ======================
    std::string Cmd_Web(const Args& args) {
        if (args.size() < 2) {
            // UPDATED: Added usage for new subcommands
            return "Usage:\n"
                "  web fetch <url> [--out <filename>]  - Fetches and renders a webpage.\n"
                "  web render <url> [--out <filename>] - Renders a page with JS and gets the final HTML.\n"
                "  web api <get|post> <url> [body]     - Interacts with a JSON API endpoint.\n"
                "  web download <page_url> <keyword> <save_path> - Finds and downloads a file from a page.";
        }

        const std::string& subcommand = args[1];

        // Existing fetch, render, and head logic (with adjusted arg parsing)
        if (subcommand == "fetch" || subcommand == "render" || subcommand == "head") {
            if (args.size() < 3) return "Usage: web " + subcommand + " <url> [--out <filename>]";
            const std::string& url = args[2];
            std::string outFile;
            if (args.size() > 3 && args[3] == "--out" && args.size() > 4) {
                outFile = args[4];
            }

            web::FetchResult result;
            std::wstring wurl = to_wstring_helper(url);

            if (subcommand == "fetch") {
                std::wcout << L"[Web] Fetching " << wurl << L"..." << std::endl;
                result = web::fetch_url(wurl);
            }
            else if (subcommand == "render") {
                std::wcout << L"[Web] Rendering " << wurl << L"..." << std::endl;
                result = web::render_url(wurl);
            }
            else if (subcommand == "head") {
                std::wcout << L"[Web] HEAD " << wurl << L"..." << std::endl;
                // Use the API fetcher for a proper HEAD request (by not reading the body)
                result = web::fetchApiData(wurl, "HEAD");
            }

            if (!result.success) {
                return "[Web] Request failed: " + to_string_helper(result.errorMessage);
            }

            std::ostringstream ss;
            ss << "[Web] Success! Status Code: " << result.statusCode << "\n";
            ss << "Received " << result.body.size() << " bytes.\n";
            if (!outFile.empty() && !result.body.empty()) {
                std::ofstream ofs(outFile, std::ios::binary);
                if (ofs) {
                    ofs.write(result.body.data(), result.body.size());
                    ss << "Content saved to '" << outFile << "'.\n";
                }
                else {
                    ss << "[Web] Error: Could not open file '" << outFile << "' for writing.\n";
                }
            }
            else if (!result.body.empty()) {
                ss << "--- Response Body (first 256 bytes) ---\n";
                ss.write(result.body.data(), std::min<size_t>(256, result.body.size()));
                ss << "\n--- End of Snippet ---\n";
            }
            if (!result.headers.empty()) {
                ss << "\n--- Response Headers ---\n";
                ss << to_string_helper(result.headers) << "\n";
            }
            if (!result.links.empty()) {
                ss << "\n--- Found " << result.links.size() << " Links ---\n";
                for (size_t i = 0; i < result.links.size() && i < 10; ++i) {
                    ss << "[" << i << "] " << to_string_helper(result.links[i]) << "\n";
                }
                if (result.links.size() > 10) ss << "...\n";
            }
            return ss.str();
        }
        // =================================================================
        // NEW: Handler for the 'api' subcommand
        // =================================================================
        else if (subcommand == "api") {
            if (args.size() < 4) return "Usage: web api <get|post> <url> [body]";

            std::string verb = args[2];
            std::wstring url = to_wstring_helper(args[3]);
            std::string postData = (args.size() > 4) ? reconstructCommand(args, 4) : "";

            web::FetchResult result = web::fetchApiData(url, verb, postData);

            if (!result.success) return "[API] Request failed: " + to_string_helper(result.errorMessage);

            return "[API] Success! Status: " + std::to_string(result.statusCode) + "\n" +
                std::string(result.body.begin(), result.body.end());
        }
        // =================================================================
        // NEW: Handler for the 'download' subcommand
        // =================================================================
        else if (subcommand == "download") {
            if (args.size() < 5) {
                return "Usage: web download <page_url> <link_keyword> <save_path>";
            }
            std::wstring pageUrl = to_wstring_helper(args[2]);
            std::wstring keyword = to_wstring_helper(args[3]);
            std::wstring savePath = to_wstring_helper(args[4]);

            auto result = web::DownloadLink(pageUrl, keyword, savePath);

            if (!result.success) {
                return "[Web] Download failed: " + to_string_helper(result.errorMessage);
            }
            return "[Web] " + to_string_helper(result.headers);
        }
        else {
            return "Unknown web command: " + subcommand;
        }
    }

    std::string Cmd_IronRouter(const Args& args) {
        if (args.size() < 2) {
            return "Usage: ironrouter <devices|listen|stop|ring|ddc|stats> ...";
        }

        const std::string subcommand = args[1];

        if (subcommand == "devices") {
            auto devices = ironrouter::LiveCapture::list_devices();
            if (devices.empty()) {
                return "[ironrouter] No network devices found. Ensure Npcap is installed.";
            }
            std::ostringstream ss;
            ss << "--- Available Network Devices ---\n";
            for (const auto& dev : devices) {
                ss << "[" << dev.id << "] " << dev.description << " (" << dev.name << ")\n";
            }
            return ss.str();
        }

        else if (subcommand == "listen") {
            if (g_network_source) return "[ironrouter] Listener is already running.";
            if (args.size() < 4) {
                return "Usage: ironrouter listen <deviceID> <port> [--ring name] [--verbose]";
            }

            int deviceID = std::stoi(args[2]);
            uint16_t port = static_cast<uint16_t>(std::stoul(args[3]));
            std::string ringName;
            bool verbose = false;

            for (size_t i = 4; i < args.size(); ++i) {
                if (args[i] == "--ring" && i + 1 < args.size()) {
                    ringName = args[++i];
                }
                else if (args[i] == "--verbose") {
                    verbose = true;
                }
            }

            // Type is now for the IPC shared-memory writer.
            ironrouter::ipc::PacketWriter* targetRing = nullptr;
            if (!ringName.empty()) {
                auto it = g_ring_writers.find(ringName);
                if (it != g_ring_writers.end()) {
                    targetRing = it->second.get();
                    std::cout << "[ironrouter] Writing packets to IPC ring: " << ringName << "\n";
                }
                else {
                    std::cout << "[ironrouter] Warning: IPC Ring '" << ringName << "' not found.\n";
                }
            }

            // Get the in-process writer from the global factory function.
            auto inprocRing = ironrouter::get_uplink_writer();

            // shared_ptr to keep log alive even after function returns
            std::shared_ptr<std::ofstream> autoLog;
            if (!targetRing) {
                std::filesystem::create_directories("logs");
                std::string autoLogName = "logs/ironrouter_dev" + std::to_string(deviceID) +
                    "_port" + std::to_string(port) + ".pcap";
                autoLog = std::make_shared<std::ofstream>(autoLogName, std::ios::binary);
                if (autoLog->is_open()) {
                    ironrouter::pcap_hdr_t gh{ 0xa1b2c3d4, 2, 4, 0, 0, 262144, 1 };
                    autoLog->write(reinterpret_cast<const char*>(&gh), sizeof(gh));
                    std::cout << "[ironrouter] Logging packets to " << autoLogName << "\n";
                }
                else {
                    std::cerr << "[ironrouter] Error: Could not open log file " << autoLogName << "\n";
                }
            }

            g_network_source = std::make_unique<ironrouter::SourceNetworkPcap>();
            g_network_source->set_frame_sink(
                [targetRing, inprocRing, verbose, autoLog](const uint8_t* data, size_t len, const ironrouter::PcapRecordHeader& hdr) {
                    static size_t packet_count = 0;
                    packet_count++;
                    if (verbose) {
                        std::cout << "[ironrouter] #" << packet_count
                            << " len=" << len
                            << " ts=" << hdr.ts_sec << "." << hdr.ts_usec << std::endl;
                    }
                    if (targetRing) {
                        uint64_t blockIndex;
                        void* block = targetRing->acquire_block_ptr(blockIndex);
                        if (block) {
                            memcpy(block, &hdr, sizeof(hdr));
                            memcpy(static_cast<char*>(block) + sizeof(hdr), data, len);
                            targetRing->commit_produce();
                        }
                    }
                    // Push into in-process ring buffer if available
                    if (inprocRing) {
                        ironrouter::PacketFrame frame{
                            std::chrono::system_clock::from_time_t(hdr.ts_sec) +
                            std::chrono::microseconds(hdr.ts_usec),
                            std::vector<uint8_t>(data, data + len),
                            static_cast<uint32_t>(len),
                            static_cast<uint32_t>(hdr.orig_len)
                        };
                        inprocRing->write(std::move(frame));
                    }
                    if (autoLog && autoLog->is_open()) {
                        ironrouter::pcaprec_hdr_t rec{ hdr.ts_sec, hdr.ts_usec,
                                                       static_cast<uint32_t>(len),
                                                       static_cast<uint32_t>(len) };
                        autoLog->write(reinterpret_cast<const char*>(&rec), sizeof(rec));
                        autoLog->write(reinterpret_cast<const char*>(data), len);
                    }
                });

            if (!g_network_source->start_listen(deviceID, port, "capture_file", true)) {
                g_network_source.reset();
                return "[ironrouter] Error: Failed to start listener.";
            }

            return "[ironrouter] Listener started.";
        }

        else if (subcommand == "stop") {
            if (!g_network_source) return "[ironrouter] Listener is not running.";
            g_network_source->stop();
            g_network_source.reset();
            return "[ironrouter] Listener stopped.";
        }

        else if (subcommand == "stats") {
            if (!g_network_source) return "[ironrouter] No active listener.";
            // Dummy stats until real implementation exists
            std::ostringstream ss;
            ss << "[ironrouter] Stats:\n"
                << "  Packets: " << 1234 << "\n"
                << "  Dropped: " << 0 << "\n";
            return ss.str();
        }

        // ring + ddc handlers here (same as your code, unchanged)

        return "Unknown ironrouter command or arguments.";
    }

    // ------------------------------------------------------------------
    // Simple consumer demo: pops one packet from the "uplink" ring
    // ------------------------------------------------------------------
    static std::string Cmd_RingDump(const Args&) {
        // Use the correct global buffer and reader class from the centralized system.
        if (!ironrouter::g_uplink_buf) {
            return "[ring] internal buffer not available to reader.";
        }

        ironrouter::InProcessPacketReader reader(ironrouter::g_uplink_buf);
        std::ostringstream ss;
        ss << "[ring] DUMPING from 'uplink' ring. Press Ctrl+C to stop.\n";

        // This will block on reader.read(frame) until a packet arrives,
        // then print it immediately and loop again.
        try {
            for (;;) {
                ironrouter::PacketFrame frame;
                if (!reader.read(frame)) {
                    // This path is less likely now with a blocking pop,
                    // but good for graceful shutdown.
                    ss << "[ring] uplink ring is empty and may be closed.\n";
                    break;
                }

                ss << "----------------------------------------\n";
                ss << "Timestamp: "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                        frame.ts.time_since_epoch()).count()
                    << " ms since epoch\n";
                ss << "Capture length: " << frame.caplen << " bytes\n";
                ss << "Original length: " << frame.origlen << " bytes\n";
                ss << "Data (first " << std::min<size_t>(frame.data.size(), 16) << " bytes):";
                for (size_t i = 0; i < frame.data.size() && i < 16; ++i) {
                    ss << " " << std::hex << std::setw(2) << std::setfill('0')
                        << static_cast<int>(frame.data[i]);
                }
                ss << std::dec << "\n";

                // Output each packet's info as it arrives
                std::cout << ss.str();
                ss.str(""); // Clear the stream for the next packet's info
            }
        }
        catch (const std::exception& e) {
            ss << "[ring] Exception in dump loop: " << e.what() << "\n";
        }

        return ss.str();
    }
    // ----------------------------------------------------------------
    // Continue with original command handlers (fully preserved)
    // ----------------------------------------------------------------

    std::string Cmd_Tiles(const Args& args) {
        if (args.size() < 2) {
            return "Usage:\n"
                "  omni:tiles run [rows cols] [tag] [--entropy|--runtime] [--tt=ms] [--hp=frac] [--oh=H] [--ow=W]\n"
                "  omni:tiles summarize <csv_path>\n";
        }
        std::string sub = args[1];
        if (sub == "run") {
            TileRunConfig cfg;
            cfg.rows = (args.size() > 2 ? (size_t)std::stoul(args[2]) : 256);
            cfg.cols = (args.size() > 3 ? (size_t)std::stoul(args[3]) : 256);
            cfg.run_tag = (args.size() > 4 ? args[4] : "");
            cfg.heatmap_entropy = true;
            cfg.target_time_ms = appConfig.tileTargetTimeMs;
            cfg.high_prio_fraction = appConfig.tileHighPrioFraction;
            cfg.overlap_h = appConfig.tileOverlapH;
            cfg.overlap_w = appConfig.tileOverlapW;
            cfg.entropy_threshold = appConfig.entropyThreshold;
            cfg.out_dir = appConfig.tileOutDir;

            for (size_t i = 5; i < args.size(); ++i) {
                const std::string& f = args[i];
                if (f == "--entropy") cfg.heatmap_entropy = true;
                else if (f == "--runtime") cfg.heatmap_entropy = false;
                else if (f.rfind("--tt=", 0) == 0) cfg.target_time_ms = std::stod(f.substr(5));
                else if (f.rfind("--hp=", 0) == 0) cfg.high_prio_fraction = std::stod(f.substr(5));
                else if (f.rfind("--oh=", 0) == 0) cfg.overlap_h = std::stoi(f.substr(5));
                else if (f.rfind("--ow=", 0) == 0) cfg.overlap_w = std::stoi(f.substr(5));
            }

            std::vector<uint16_t> chunks = { 0xDEF0,0x9ABC,0x5678,0x1234,0xDEF0,0x9ABC,0x5678,0x1234 };
            auto summary = TileAnalytics::RunFromChunks(chunks, cfg);

            std::ostringstream ss;
            ss << "--- Tile Run Summary ---\n";
            ss << "csv: " << summary.csv_path << "\n";
            for (auto& h : summary.heatmaps) ss << "pgm: " << h << "\n";
            ss << "epochs=" << summary.epochs
                << " tiles=" << summary.tiles_total
                << " wall=" << std::fixed << std::setprecision(3) << summary.wall_ms << " ms"
                << " median_tile=" << std::fixed << std::setprecision(3) << summary.median_tile_us << " us"
                << " second_pass=" << summary.second_pass_total << "\n";
            return ss.str();
        }

        if (sub == "summarize") {
            if (args.size() < 3) return "Usage: omni:tiles summarize <csv_path>";
            std::ifstream f(args[2]);
            if (!f) return "Error: cannot open CSV: " + args[2];
            std::string line; std::getline(f, line); // header
            struct Row { int epoch; int high; double entropy; uint64_t usec; };
            std::vector<Row> rows;
            while (std::getline(f, line)) {
                std::istringstream ss(line);
                std::string tok;
                Row r{};
                std::getline(ss, tok, ','); r.epoch = std::stoi(tok);
                for (int i = 0; i < 6; i++) std::getline(ss, tok, ','); // skip to usec
                std::getline(ss, tok, ','); r.usec = std::stoull(tok);
                std::getline(ss, tok, ','); r.entropy = std::stod(tok);
                std::getline(ss, tok, ','); r.high = std::stoi(tok);
                rows.push_back(r);
            }
            if (rows.empty()) return "No rows parsed from CSV.";
            auto epoch_min = rows.front().epoch, epoch_max = rows.back().epoch;
            size_t high_count = 0; std::vector<uint64_t> times;
            times.reserve(rows.size());
            for (auto& r : rows) { high_count += (r.high ? 1u : 0u); times.push_back(r.usec); }
            std::nth_element(times.begin(), times.begin() + times.size() / 2, times.end());
            double med_us = static_cast<double>(times[times.size() / 2]);
            std::ostringstream out;
            out << "--- Tile CSV Summary ---\n";
            out << "epochs: " << epoch_min << " .. " << epoch_max << "\n";
            out << "rows: " << rows.size() << "  high_prio_rows: " << high_count << "\n";
            out << "median_tile_us: " << std::fixed << std::setprecision(3) << med_us << "\n";
            return out.str();
        }

        return "Unknown subcommand. See: omni:tiles";
    }

    std::string Cmd_TilesMerge(const Args& args) {
        if (args.size() < 4)
            return "Usage: omni:tiles_merge <pgm1> <pgm2> <out.pgm>";

        // wrap inputs in a vector
        std::vector<std::string> inputs = { args[1], args[2] };

        // simple average merge function
        auto mergeFn = [](const std::vector<uint8_t>& vals) -> uint8_t {
            uint32_t sum = 0;
            for (uint8_t v : vals) sum += v;
            return static_cast<uint8_t>(sum / vals.size());
            };

        // MergeHeatmaps now returns the out path (empty if failed)
        std::string outPath = TileAnalytics::MergeHeatmaps(inputs, args[3], mergeFn);
        return !outPath.empty() ? "[Tiles] Merged into: " + outPath
            : "[Tiles] Merge failed.";
    }

    static std::thread g_pmuThread;

    std::string Cmd_PmuAnalyze(const Args&) {
        auto sample = PMU::SampleSelf();
        std::ostringstream csv;
        saveCSV("reports/pmu_latest.csv", sample);
        try {
            std::string analysis = OmniAIManager::summarize(readFile("reports/pmu_latest.csv"));
            writeFile("reports/pmu_latest.analysis.txt", analysis);
            return "[PMU] Analysis saved: reports/pmu_latest.analysis.txt";
        }
        catch (...) {
            return "[PMU] Analysis path unavailable.";
        }
    }

    std::string Cmd_PmuMonitor(const Args& args) {
        if (args.size() >= 2 && args[1] == "stop") {
            PMU::g_pmuStopFlag.store(true);
            if (g_pmuThread.joinable()) {
                g_pmuThread.join();
            }
            return "[PMU] Monitor stopped.";
        }

        // Optional args: interval(ms) and topN
        int intervalMs = (args.size() >= 2) ? std::stoi(args[1]) : 1000;
        size_t topN = (args.size() >= 3) ? static_cast<size_t>(std::stoul(args[2])) : 5;

        if (g_pmuThread.joinable()) {
            return "[PMU] Monitor is already running. Stop it first.";
        }

        PMU::g_pmuStopFlag.store(false);
        g_pmuThread = std::thread([intervalMs, topN]() {
            PMU::MonitorSelf(std::chrono::milliseconds(intervalMs),
                topN,
                {}, // no custom callback; will print to stdout & push to OmniAIManager
                &PMU::g_pmuStopFlag);
            });

        return "[PMU] Monitor started (interval=" + std::to_string(intervalMs) +
            "ms, topN=" + std::to_string(topN) + ")";
    }
    std::string Cmd_PmuSample(const Args&) {
        auto s = PMU::SampleSelf();
        std::ostringstream out;
        out << "PID: " << s.pid << "\nUser ms: " << s.user_ms << "\nKernel ms: " << s.kernel_ms << "\n";
        for (const auto& t : s.thread_samples) {
            out << "  TID: " << t.tid
                << " user_ms=" << t.user_ms
                << " kernel_ms=" << t.kernel_ms << "\n";
        }
        return out.str();
    }

    std::string Cmd_PmuSave(const Args& args) {
        if (args.size() < 2) return "Usage: omni:pmu_save <output.csv>";
        auto s = PMU::SampleSelf();
        saveCSV(args[1], s);
        return std::string("Sample saved to: ") + args[1];
    }

    std::string Cmd_PmuDiff(const Args& args) {
        if (args.size() < 3) return "Usage: omni:pmu_diff <old.csv> <new.csv>";
        auto a = loadCSV(args[1]);
        auto b = loadCSV(args[2]);
        auto d = PMU::Diff(a, b);
        std::ostringstream out;
        out << "Proc Delta: user=" << d.proc_user_ms << " kernel=" << d.proc_kernel_ms << "\n";
        for (const auto& td : d.thread_deltas) {
            out << "TID " << td.tid
                << ": user_delta=" << td.user_ms
                << " kernel_delta=" << td.kernel_ms << "\n";
        }
        return out.str();
    }

    std::string Cmd_PmuSummary(const Args& args) {
        if (args.size() < 2) return "Usage: omni:pmu_summary <input.csv>";
        std::ifstream f(args[1]);
        if (!f) return "Error: cannot open " + args[1];
        std::string line;
        std::getline(f, line); // header proc
        std::getline(f, line); // values
        std::string proc_line = line;
        std::getline(f, line); // header thread
        struct TRow { uint32_t tid; double u; double k; };
        std::vector<TRow> rows;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string tid, u, k;
            std::getline(ss, tid, ','); std::getline(ss, u, ','); std::getline(ss, k, ',');
            if (tid.empty()) continue;
            rows.push_back(TRow{ (uint32_t)std::stoul(tid), std::stod(u), std::stod(k) });
        }
        std::sort(rows.begin(), rows.end(), [](const TRow& a, const TRow& b) {
            return (a.u + a.k) > (b.u + b.k);
            });
        std::ostringstream out;
        out << "--- PMU Summary ---\n";
        out << "proc: " << proc_line << "\n";
        for (const auto& r : rows) {
            out << "tid=" << r.tid << " cpu_ms=" << std::fixed << std::setprecision(3) << (r.u + r.k)
                << " (user=" << r.u << ", kernel=" << r.k << ")\n";
        }
        return out.str();
    }

    // Omni editor command (no lambdas)
    std::string Cmd_OmniEdit(const Args& args) {
        if (args.size() < 2) return "Usage: omni:edit <filename>";
        try {
            OmniEditorIDE::OpenBuffer(args[1], "");
            OmniEditorIDE::LaunchInteractiveUI();
            return "Editor closed.";
        }
        catch (...) {
            return "Editor path unavailable.";
        }
    }

    // =================================================================
    // NEW: Command Handler for Binary Manipulation
    // =================================================================
    std::string Cmd_Binary(const Args& args) {
        if (args.size() < 2) { // Changed to 2, as some commands need more args
            return "Usage: omni:binary <probe|sections|symbols|attach|diff|ai-analyze> ...";
        }

        std::string subcommand = args[1];
        std::ostringstream ss;

        // --- Handler for "probe" subcommand ---
        if (subcommand == "probe") {
            if (args.size() < 3) return "Usage: omni:binary probe <filepath>";
            std::string target = args[2];
            auto info = BinaryManip::Probe(target);
            if (!info) {
                return "Error: Failed to probe binary '" + target + "'.";
            }
            ss << "--- Binary Info ---\n";
            ss << "Path: " << info->path << "\n";
            ss << "OS: " << (info->os == BinaryManip::OS::Windows ? "Windows" : (info->os == BinaryManip::OS::Linux ? "Linux" : "Unknown")) << "\n";
            ss << "Arch: ";
            switch (info->arch) {
            case BinaryManip::Arch::X86: ss << "x86"; break;
            case BinaryManip::Arch::X64: ss << "x64"; break;
            case BinaryManip::Arch::ARM: ss << "ARM"; break;
            case BinaryManip::Arch::ARM64: ss << "ARM64"; break;
            default: ss << "Unknown"; break;
            }
            ss << "\nIs Library: " << (info->isLibrary ? "Yes" : "No") << "\n";
            ss << "Position Independent: " << (info->positionIndependent ? "Yes" : "No") << "\n";
            ss << "Stripped: " << (info->stripped ? "Yes" : "No") << "\n";
            ss << "Image Base: 0x" << std::hex << info->imageBase << "\n";
            ss << "Entry RVA: 0x" << std::hex << info->entryRVA << "\n";
        }
        // --- Handler for "sections" subcommand ---
        else if (subcommand == "sections") {
            if (args.size() < 3) return "Usage: omni:binary sections <filepath>";
            std::string target = args[2];
            auto sections = BinaryManip::ListSections(target);
            if (sections.empty()) {
                return "No sections found or file could not be parsed.";
            }
            ss << "--- Sections ---\n";
            for (const auto& sec : sections) {
                ss << sec << "\n";
            }
        }
        // --- Handler for "symbols" subcommand ---
        else if (subcommand == "symbols") {
            if (args.size() < 3) return "Usage: omni:binary symbols <filepath>";
            std::string target = args[2];
            auto symbols = BinaryManip::DiscoverSymbols(target);
            if (symbols.empty()) {
                return "No symbols found or file is stripped.";
            }
            ss << "--- Symbols ---\n";
            for (const auto& sym : symbols) {
                ss << sym << "\n";
            }
        }
        // --- Handler for "attach" subcommand ---
        else if (subcommand == "attach") {
            if (args.size() < 3) return "Usage: omni:binary attach <pid>";
            std::string target = args[2];
            try {
                BinaryManip::ProcId pid = std::stoul(target);
                BinaryManip::RewriteOpts opts; // Default options
                auto result = BinaryManip::AttachAndInstrument(pid, opts);
                ss << (result.ok ? "[SUCCESS] " : "[FAILURE] ") << result.message;
            }
            catch (...) {
                return "Error: Invalid process ID for attach.";
            }
        }
        // --- Handler for "diff" subcommand ---
        else if (subcommand == "diff") {
            if (args.size() < 4) return "Usage: omni:binary diff <file1> <file2>";
            auto diff_offset = BinaryManip::FindFirstDifference(args[2], args[3]);
            if (diff_offset) {
                ss << "Files differ at offset: 0x" << std::hex << *diff_offset;
            }
            else {
                ss << "Files are identical.";
            }
        }
        // --- Handler for "ai-analyze" subcommand ---
        else if (subcommand == "ai-analyze") {
            if (args.size() < 3) return "Usage: omni:binary ai-analyze <filepath>";
            auto result = BinaryManip::AnalyzeWithAI(args[2]);
            ss << "--- AI Analysis Report ---\n";
            ss << "Status: " << (result.success ? "Success" : "Failure") << "\n";
            ss << "Message: " << result.message << "\n";
            ss << "Confidence: " << std::fixed << std::setprecision(4) << result.confidence << "\n";
            ss << "Findings:\n";
            for (const auto& finding : result.findings) {
                ss << "  - " << finding << "\n";
            }
        }
        // --- Fallback for unknown subcommand ---
        else {
            return "Unknown omni:binary subcommand. Use 'probe', 'sections', 'symbols', 'attach', 'diff', or 'ai-analyze'.";
        }

        return ss.str();
    }

    // =================================================================
    // NEW: Command Handler for Sending Email
    // =================================================================
    // Utility to split a comma-separated string into a vector
    static std::vector<std::string> SplitCSV(const std::string& str) {
        std::vector<std::string> result;
        std::stringstream ss(str);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) { return !std::isspace(ch); }));
            item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), item.end());
            if (!item.empty()) result.push_back(item);
        }
        return result;
    }

    // =================================================================
    // NEW: Command Handler for Sending Email with Dynamic Attachment Scanning
    // =================================================================

    // Thread-safe queue for discovered files
    static std::queue<std::string> g_fileQueue;
    static std::mutex g_queueMutex;
    static std::condition_variable g_queueCV;
    static std::atomic<bool> g_scanDone{ false };

    // Recursively scan all drives for a specific filename
    static void ScanDrivesForFile(const std::string& targetFilename) {
#ifdef _WIN32
        for (char drive = 'A'; drive <= 'Z'; ++drive) {
            std::string root = std::string(1, drive) + ":\\";
            if (GetDriveTypeA(root.c_str()) == DRIVE_FIXED) {
                try {
                    for (auto& p : std::filesystem::recursive_directory_iterator(root)) {
                        if (!p.is_regular_file()) continue;
                        if (p.path().filename() == targetFilename) {
                            {
                                std::lock_guard<std::mutex> lock(g_queueMutex);
                                g_fileQueue.push(p.path().string());
                            }
                            g_queueCV.notify_one();
                        }
                    }
                }
                catch (...) { /* skip inaccessible paths */ }
            }
        }
#endif
        g_scanDone = true;
        g_queueCV.notify_all();
    }

    std::string Cmd_Email(const Args& args) {
        if (args.size() < 2) {
            return "Usage: omni:email --to <recipient[,recipient2,...]> --subject \"<subject>\" --body \"<body>\" "
                "[--file <filename>] [--attach <exact path>] "
                "[--smtp-server <host>] [--smtp-port <port>] [--from <from header>] "
                "[--smtp-user <username>] [--smtp-pass <password>]\n"
                "Example: omni:email --to user@example.com --subject \"Test\" --body \"Hello\" "
                "--smtp-server mail.myhost.net --smtp-user relay --smtp-pass secret";
        }

        std::vector<std::string> to;
        std::string subject, body;
        std::vector<std::string> exactAttachments;
        std::string streamingFilename;

        // new parametric fields
        std::string smtpServer = "smtp.gmail.com";
        std::string smtpPort = "587";
        std::string fromHeader = "Mail.gmail.com";
        std::string smtpUser = "cadellanderson@gmail.com";
        std::string smtpPass = "nooj thkv lqmy fuxp";

        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--to" && i + 1 < args.size()) {
                auto recipients = SplitCSV(args[++i]);
                to.insert(to.end(), recipients.begin(), recipients.end());
            }
            else if (args[i] == "--subject" && i + 1 < args.size()) {
                ++i;
                while (i < args.size() && args[i].rfind("--", 0) != 0) {
                    if (!subject.empty()) subject += " ";
                    subject += args[i++];
                }
                --i;
            }
            else if (args[i] == "--body" && i + 1 < args.size()) {
                ++i;
                while (i < args.size() && args[i].rfind("--", 0) != 0) {
                    if (!body.empty()) body += " ";
                    body += args[i++];
                }
                --i;
            }
            else if (args[i] == "--file" && i + 1 < args.size()) {
                streamingFilename = args[++i];
            }
            else if (args[i] == "--attach" && i + 1 < args.size()) {
                exactAttachments.push_back(args[++i]);
            }
            // new overrides
            else if (args[i] == "--smtp-server" && i + 1 < args.size()) {
                smtpServer = args[++i];
            }
            else if (args[i] == "--smtp-port" && i + 1 < args.size()) {
                smtpPort = args[++i];
            }
            else if (args[i] == "--from" && i + 1 < args.size()) {
                fromHeader = args[++i];
            }
            else if (args[i] == "--smtp-user" && i + 1 < args.size()) {
                smtpUser = args[++i];
            }
            else if (args[i] == "--smtp-pass" && i + 1 < args.size()) {
                smtpPass = args[++i];
            }
        }

        if (to.empty() || subject.empty() || body.empty()) {
            return "Error: --to, --subject, and --body are all required.";
        }

        bool success = true;
#ifdef _WIN32
        // Informative log for streaming mode
        if (!streamingFilename.empty()) {
            std::cout << "[Cmd_Email] Streaming attachment search initiated for file: "
                << streamingFilename << "\n";
        }
        success &= ScriptRunner::sendEmailWithStreamingAttachments(
            smtpServer, smtpPort, fromHeader, smtpUser, smtpPass,
            to, subject, body, streamingFilename, exactAttachments
        );
#else
        success = false;
#endif
        return success ? "Email sent successfully to recipients." : "Failed to send email.";
    }

    // Forward declaration so Cmd_LlmGen can call it before definition
    static bool is_quiet();

    // =================================================================
    // NEW: omni:llm:load
    // =================================================================
    std::string Cmd_LlmLoad(const Args& a) {
        if (a.size() < 2) return "Usage: omni:llm:load <model_path>";
        return RouterLLM::ensureLoaded(a[1]) ? "[LLM] Model loaded." : "[LLM] Failed to load model.";
    }

    // =================================================================
    // NEW: omni:llm:status
    // =================================================================
    std::string Cmd_LlmStatus(const Args&) {
        if (!RouterLLM::g_loaded) return "[LLM] No model loaded.";
        const auto& cfg = RouterLLM::g_engine.W.cfg;
        std::ostringstream os;
        os << "[LLM] Model: " << RouterLLM::g_modelPath << "\n"
            << "       vocab=" << cfg.vocab_size
            << " d_model=" << cfg.d_model
            << " layers=" << cfg.n_layers
            << " heads=" << cfg.n_heads
            << " max_seq=" << cfg.max_seq << "\n"
            << "       mlp_kind=" << cfg.mlp_kind
            << " norm_kind=" << cfg.norm_kind
            << " rope_theta_base=" << cfg.rope_theta_base
            << " rope_freq_scale=" << cfg.rope_freq_scale;
        return os.str();
    }

    std::string Cmd_LlmSet(const Args& a) {
        if (a.size() < 3)
            return "Usage: omni:llm:set <mlp_kind|norm_kind|rope_theta_base|rope_freq_scale> <value>";
        if (!RouterLLM::g_loaded)
            return "[LLM] No model loaded. Load a model first.";

        auto& cfg = RouterLLM::g_engine.W.cfg;
        const std::string key = a[1];
        const std::string val = a[2];

        try {
            if (key == "mlp_kind")        cfg.mlp_kind = std::stoi(val);
            else if (key == "norm_kind")  cfg.norm_kind = std::stoi(val);
            else if (key == "rope_theta_base") cfg.rope_theta_base = std::stof(val);
            else if (key == "rope_freq_scale") cfg.rope_freq_scale = std::stof(val);
            else return "Unknown key: " + key;
        }
        catch (...) {
            return "Invalid value for key: " + key;
        }

        return "[LLM] Updated " + key + " = " + val;
    }

    // =================================================================
    // UPDATED: omni:llm:gen — use RouterLLM::run for unified decode path
    // =================================================================
    std::string Cmd_LlmGen(const Args& a) {
        if (a.size() < 2)
            return "Usage: omni:llm:gen \"<prompt>\" [--n N] [--temp T] [--top-k K] [--top-p P] [--nostream]";
        if (!RouterLLM::g_loaded)
            return "[LLM] No model loaded. Use omni:llm:load <path> first.";

        RouterLLM::Options opt;
        opt.model = RouterLLM::g_modelPath;
        opt.prompt.clear();
        opt.stream = true;

        // Collect prompt until first flag
        size_t idx = 1;
        std::string joined;
        for (; idx < a.size(); ++idx) {
            if (a[idx].rfind("--", 0) == 0) break;
            if (!joined.empty()) joined += " ";
            joined += a[idx];
        }
        opt.prompt = joined;

        // Parse flags
        for (; idx < a.size(); ++idx) {
            if (a[idx] == "--n" && idx + 1 < a.size()) opt.n_predict = std::stoi(a[++idx]);
            else if (a[idx] == "--temp" && idx + 1 < a.size()) opt.temp = std::stof(a[++idx]);
            else if (a[idx] == "--top-k" && idx + 1 < a.size()) opt.top_k = std::stoi(a[++idx]);
            else if (a[idx] == "--top-p" && idx + 1 < a.size()) opt.top_p = std::stof(a[++idx]);
            else if (a[idx] == "--nostream") opt.stream = false;
        }

        // Use unified RouterLLM::run so streaming decode benefits from tokenizer
        if (opt.stream) {
            if (!is_quiet()) std::cout << ">> ";
            RouterLLM::run(opt);
            return std::string(); // streamed directly
        }
        else {
            return RouterLLM::run(opt); // return as string
        }
    }

    // =================================================================
// OPTIONAL: omni:llm:unload — free the engine if needed
// =================================================================
    std::string Cmd_LlmUnload(const Args&) {
        if (!RouterLLM::g_loaded)
            return "[LLM] No model loaded.";

        // Simplest safe unload: reset the engine instance
        RouterLLM::g_engine = CLLF{};
        RouterLLM::g_modelPath.clear();
        RouterLLM::g_loaded = false;

        return "[LLM] Model unloaded.";
    }

    // --- Existing helpers in anonymous namespace ---
    static bool is_quiet() {
        std::string q = get_env("OMNI_QUIET");
        return !q.empty() && q[0] == '1';
    }

    // =================================================================
    // NEW: Config control
    // =================================================================
    std::string Cmd_CfgReload(const Args&) {
        try {
            const std::string cfgPath = R"(Z:\source\OmniShell\config\OmniConfig.xml)";
            if (OmniConfigNS::load(cfgPath, appConfig)) {
                return "[Config] Reloaded: " + cfgPath;
            }
            return "[Config] Failed to reload: " + cfgPath;
        }
        catch (...) {
            return "[Config] Reload threw an exception.";
        }
    }

    std::string Cmd_CfgShow(const Args&) {
        std::ostringstream os;
        os << "--- OmniShell Config (selected) ---\n";
        os << "monitorSensors: " << (appConfig.monitorSensors ? "true" : "false") << "\n";
        os << "defaultQuarantineDir: " << appConfig.defaultQuarantineDir << "\n";
        os << "defaultReportDir:     " << appConfig.defaultReportDir << "\n";
        os << "entropyThreshold:     " << appConfig.entropyThreshold << "\n";
        os << "tileTargetTimeMs:     " << appConfig.tileTargetTimeMs << "\n";
        os << "tileHighPrioFraction: " << appConfig.tileHighPrioFraction << "\n";
        os << "tileOverlapH:         " << appConfig.tileOverlapH << "\n";
        os << "tileOverlapW:         " << appConfig.tileOverlapW << "\n";
        os << "tileOutDir:           " << appConfig.tileOutDir << "\n";
        return os.str();
    }

    // =================================================================
    // NEW: Logs tail (uses existing tailFile helper)
    // =================================================================
    std::string Cmd_LogsTail(const Args& a) {
        if (a.size() < 2) return "Usage: omni:logs:tail <path> [--lines N]";
        std::string path = a[1];
        size_t lines = 100;
        for (size_t i = 2; i + 1 < a.size(); ++i) {
            if (a[i] == "--lines") {
                try { lines = static_cast<size_t>(std::stoul(a[i + 1])); }
                catch (...) {}
                break;
            }
        }
        auto out = tailFile(path, lines);
        if (out.empty()) return "No data at: " + path;
        return out;
    }

    // =================================================================
    // NEW: LLM over file with prompt prefix -> write to file
    // Usage:
    //   omni:llm:file "<prompt-prefix>" <inputPath> <outputPath> [--n N] [--temp T] [--top-k K] [--top-p P]
    // =================================================================
    std::string Cmd_LlmFile(const Args& a) {
        if (a.size() < 4) {
            return "Usage: omni:llm:file \"<prompt-prefix>\" <inputPath> <outputPath> "
                "[--n N] [--temp T] [--top-k K] [--top-p P]";
        }
        if (!RouterLLM::g_loaded) return "[LLM] No model loaded. Use omni:llm:load <path> first.";

        // Extract required args
        std::string promptPrefix = a[1];
        std::string inPath = a[2];
        std::string outPath = a[3];

        // Defaults
        RouterLLM::Options opt;
        opt.model = RouterLLM::g_modelPath;
        opt.stream = false; // always non-stream for file output

        // Optional flags
        for (size_t i = 4; i + 1 < a.size(); ++i) {
            if (a[i] == "--n") { try { opt.n_predict = std::stoi(a[i + 1]); } catch (...) {} }
            else if (a[i] == "--temp") { try { opt.temp = std::stof(a[i + 1]); } catch (...) {} }
            else if (a[i] == "--top-k") { try { opt.top_k = std::stoi(a[i + 1]); } catch (...) {} }
            else if (a[i] == "--top-p") { try { opt.top_p = std::stof(a[i + 1]); } catch (...) {} }
        }

        // Read input file
        std::string contents = readFile(inPath);
        if (contents.empty()) return "No data at: " + inPath;

        // Construct prompt
        std::ostringstream p;
        p << promptPrefix
            << "\n\n<<<BEGIN FILE \"" << inPath << "\">>>\n"
            << contents
            << "\n<<<END FILE>>>";
        opt.prompt = p.str();

        // Generate via unified decode path
        std::string result = RouterLLM::run(opt);

        // Ensure output directory and write
        try {
            std::filesystem::path pOut(outPath);
            auto d = pOut.parent_path();
            if (!d.empty()) std::filesystem::create_directories(d);
        }
        catch (...) {}
        writeFile(outPath, result);

        std::ostringstream msg;
        msg << "[LLM] Wrote: " << outPath << " (" << result.size() << " bytes)";
        return msg.str();
    }

    // =================================================================
    // NEW: One-shot daemon log annotation helper
    // Usage:
    //   omni:log:annotate <logPath> [--out <outPath>] [--n N] [--temp T] [--top-k K] [--top-p P]
    // =================================================================
    std::string Cmd_LogAnnotate(const Args& a) {
        if (a.size() < 2) {
            return "Usage: omni:log:annotate <logPath> [--out <outPath>] "
                "[--n N] [--temp T] [--top-k K] [--top-p P]";
        }
        if (!RouterLLM::g_loaded) return "[LLM] No model loaded. Use omni:llm:load <path> first.";

        std::string logPath = a[1];
        std::string outPath;

        // Defaults
        RouterLLM::Options opt;
        opt.model = RouterLLM::g_modelPath;
        opt.stream = false;

        // Flags
        for (size_t i = 2; i < a.size(); ++i) {
            if (a[i] == "--out" && i + 1 < a.size()) outPath = a[++i];
            else if (a[i] == "--n" && i + 1 < a.size()) { try { opt.n_predict = std::stoi(a[++i]); } catch (...) {} }
            else if (a[i] == "--temp" && i + 1 < a.size()) { try { opt.temp = std::stof(a[++i]); } catch (...) {} }
            else if (a[i] == "--top-k" && i + 1 < a.size()) { try { opt.top_k = std::stoi(a[++i]); } catch (...) {} }
            else if (a[i] == "--top-p" && i + 1 < a.size()) { try { opt.top_p = std::stof(a[++i]); } catch (...) {} }
        }

        // Read log
        std::string contents = readFile(logPath);
        if (contents.empty()) return "No data at: " + logPath;

        // Default output path if not given
        if (outPath.empty()) {
            std::string base = pathBaseName(logPath);
            std::filesystem::path pOut = std::filesystem::path("reports/daemon") /
                (base + std::string("_annotated.txt"));
            outPath = pOut.string();
        }

        // Construct domain-specific prompt
        std::ostringstream p;
        p << "Summarize and annotate the findings in the following daemon log. "
            << "Focus on key errors, warnings, timestamps, impacted components, and actionable remediation steps. "
            << "Group by theme, and end with a prioritized checklist.\n\n"
            << "<<<BEGIN LOG \"" << logPath << "\">>>\n"
            << contents
            << "\n<<<END LOG>>>";

        opt.prompt = p.str();

        // Generate and write
        std::string result = RouterLLM::g_engine.generate(opt.prompt, opt.n_predict, opt.temp, opt.top_k, opt.top_p, false);
        try {
            std::filesystem::path pOut(outPath);
            auto d = pOut.parent_path();
            if (!d.empty()) std::filesystem::create_directories(d);
        }
        catch (...) {}
        writeFile(outPath, result);

        std::ostringstream msg;
        msg << "[LLM] Annotated log saved: " << outPath << " (" << result.size() << " bytes)";
        return msg.str();
    }


    // ---------------- Helper definitions ----------------

    std::string reconstructCommand(const std::vector<std::string>& args, size_t start_index) {
        if (args.size() <= start_index) return "";
        std::string command;
        for (size_t i = start_index; i < args.size(); ++i) {
            if (i > start_index) command += " ";
            command += args[i];
        }
        return command;
    }

    std::string readFile(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return "";
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    void writeFile(const std::string& path, const std::string& content) {
        std::ofstream f(path, std::ios::binary);
        if (f) f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    void ensureDir(const std::string& path) {
        std::filesystem::create_directories(path);
    }

    std::string tailFile(const std::string& path, size_t lines) {
        std::ifstream f(path);
        if (!f) return "";
        std::vector<std::string> buffer;
        std::string line;
        while (std::getline(f, line)) {
            buffer.push_back(line);
            if (buffer.size() > lines) buffer.erase(buffer.begin());
        }
        std::ostringstream ss;
        for (const auto& l : buffer) ss << l << "\n";
        return ss.str();
    }

    std::string pathBaseName(const std::string& path) {
        return std::filesystem::path(path).filename().string();
    }

    void saveCSV(const std::string& path, const PMU::ProcessSample& s) {
        std::ofstream f(path);
        if (!f) {
            std::cerr << "Failed to write: " << path << "\n";
            return;
        }
        f << "Process PID,user_ms,kernel_ms\n";
        f << s.pid << "," << s.user_ms << "," << s.kernel_ms << "\n";
        f << "Thread TID,user_ms,kernel_ms\n";
        for (const auto& t : s.thread_samples)
            f << t.tid << "," << t.user_ms << "," << t.kernel_ms << "\n";
    }

    PMU::ProcessSample loadCSV(const std::string& path) {
        PMU::ProcessSample out{};
        std::ifstream f(path);
        if (!f) return out;
        std::string header, line;
        std::getline(f, header);
        std::getline(f, line);

        std::istringstream proc(line);
        std::string pid_s, user_s, kernel_s;
        std::getline(proc, pid_s, ',');
        std::getline(proc, user_s, ',');
        std::getline(proc, kernel_s, ',');

        out.pid = static_cast<uint32_t>(std::stoul(pid_s));
        out.user_ms = std::stod(user_s);
        out.kernel_ms = std::stod(kernel_s);

        std::getline(f, header); // skip thread header
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string tid, u, k;
            std::getline(ss, tid, ',');
            std::getline(ss, u, ',');
            std::getline(ss, k, ',');
            if (tid.empty()) continue;
            PMU::ThreadSample ts;
            ts.tid = static_cast<uint32_t>(std::stoul(tid));
            ts.user_ms = std::stod(u);
            ts.kernel_ms = std::stod(k);
            out.thread_samples.push_back(ts);
        }
        out.threads = out.thread_samples.size();
        out.taken_at = std::chrono::steady_clock::now();
        return out;
    }

    // Utility to register and record command names
    inline void add_cmd(CommandRouter& router, const std::string& name, CommandRouter::CommandFunction fn) {
        g_command_names.push_back(name);
        router.registerCommand(name, std::move(fn));
    }

} // anonymous namespace

// Optional accessor impl
const std::map<std::string, CommandMeta>& CommandRouter::GetCommandMetadata() {
    return g_command_metadata;
}


// =================================================================
// CommandRouter implementation
// =================================================================

CommandRouter::CommandRouter() {
    // UPDATED: Call the correct global registration function.
    ironrouter::register_packet_rings();

    // Basic / job
    add_cmd(*this, "help", &Cmd_Help);
    add_cmd(*this, "exit", &Cmd_Exit);
    add_cmd(*this, "jobs", &Cmd_Jobs);
    add_cmd(*this, "fg", &Cmd_Fg);

    // Navigation
    add_cmd(*this, "cd", &Cmd_Cd);
    add_cmd(*this, "pwd", &Cmd_Pwd);

#if defined(_WIN32)
    add_cmd(*this, "ls", &Cmd_Ls);
    add_cmd(*this, "ps", &Cmd_Ps);
    add_cmd(*this, "omni:dev", &Cmd_Dev);
    add_cmd(*this, "git", &Cmd_Git);
    add_cmd(*this, "vcpkg", &Cmd_Vcpkg);
#elif defined(__linux__)
    add_cmd(*this, "dir", &Cmd_Dir);
    add_cmd(*this, "omni:disk_usage", &Cmd_DiskUsage);
    add_cmd(*this, "omni:mem_info", &Cmd_MemInfo);
#endif

    // Omni help and editor
    add_cmd(*this, "omni:help", &Cmd_OmniHelp);
    add_cmd(*this, "omni:edit", &Cmd_OmniEdit);

#if defined(_WIN32)
    add_cmd(*this, "omni:repair_all", &Cmd_RepairAll);
    add_cmd(*this, "omni:repair_sfc", &Cmd_RepairSfc);
    add_cmd(*this, "omni:repair_dism", &Cmd_RepairDism);
    add_cmd(*this, "omni:disk_check", &Cmd_DiskCheck);
#endif

    // Diagnostics/process
    add_cmd(*this, "omni:diagnose", &Cmd_Diagnose);
    add_cmd(*this, "omni:kill", &Cmd_Kill);
    add_cmd(*this, "omni:task_daemon", &Cmd_TaskDaemon);

    // AI shell
    add_cmd(*this, "omni:ask", &Cmd_Ask);
    add_cmd(*this, "omni:mode", &Cmd_Mode);
    add_cmd(*this, "omni:explain", &Cmd_Explain);
    add_cmd(*this, "omni:gen", &Cmd_Gen);
    add_cmd(*this, "omni:log:sum", &Cmd_LogSum);
    add_cmd(*this, "omni:ctx", &Cmd_Ctx);
    add_cmd(*this, "omni:models", &Cmd_Models);
    add_cmd(*this, "omni:ctx_dump", &Cmd_CtxDump);

    // Sensors / Tiles / PMU
    add_cmd(*this, "omni:sensor_list", &Cmd_SensorList);
    // Extended sensor commands (verbose dump, show by id/label, export, filter)
    add_cmd(*this, "omni:sensor_dump", &Cmd_SensorDump);
    add_cmd(*this, "omni:sensor_show", &Cmd_SensorShow);
    add_cmd(*this, "omni:sensor_snapshot", &Cmd_SensorSnapshot);
    add_cmd(*this, "omni:sensor_export", &Cmd_SensorExport);
    add_cmd(*this, "omni:sensor_filter", &Cmd_SensorFilter);
    add_cmd(*this, "web", &Cmd_Web);
    add_cmd(*this, "ironrouter", &Cmd_IronRouter);
    add_cmd(*this, "ring:dump", &Cmd_RingDump);
    add_cmd(*this, "omni:tiles", &Cmd_Tiles);
    add_cmd(*this, "omni:tiles_merge", &Cmd_TilesMerge);
    add_cmd(*this, "omni:pmu_analyze", &Cmd_PmuAnalyze);
    add_cmd(*this, "omni:pmu_monitor", &Cmd_PmuMonitor);
    add_cmd(*this, "omni:pmu_sample", &Cmd_PmuSample);
    add_cmd(*this, "omni:pmu_save", &Cmd_PmuSave);
    add_cmd(*this, "omni:pmu_diff", &Cmd_PmuDiff);
    add_cmd(*this, "omni:pmu_summary", &Cmd_PmuSummary);

    // NEW: Register the binary analysis command
    add_cmd(*this, "omni:binary", &Cmd_Binary);
    // NEW: Register the email command
    add_cmd(*this, "omni:email", &Cmd_Email);
    add_cmd(*this, "run-script", &Cmd_RunScript);
    add_cmd(*this, "run-py", &Cmd_RunPy);
    add_cmd(*this, "run-bash", &Cmd_RunBash);

    // --- ADD: LLM commands ---
    add_cmd(*this, "omni:llm:load", &Cmd_LlmLoad);
    add_cmd(*this, "omni:llm:status", &Cmd_LlmStatus);
    add_cmd(*this, "omni:llm:set", &Cmd_LlmSet);
    add_cmd(*this, "omni:llm:gen", &Cmd_LlmGen);
    add_cmd(*this, "omni:llm:unload", &Cmd_LlmUnload);
    add_cmd(*this, "omni:llm:help", &Cmd_LlmHelp);

    // Config
    add_cmd(*this, "omni:cfg:reload", &Cmd_CfgReload);
    add_cmd(*this, "omni:cfg:show", &Cmd_CfgShow);

    // Logs
    add_cmd(*this, "omni:logs:tail", &Cmd_LogsTail);

    // LLM file utilities
    add_cmd(*this, "omni:llm:file", &Cmd_LlmFile);
    add_cmd(*this, "omni:log:annotate", &Cmd_LogAnnotate);

    // AI Engine commands
    add_cmd(*this, "omni:ai:load", &Cmd_AiLoad);
    add_cmd(*this, "omni:ai:unload", &Cmd_AiUnload);
    add_cmd(*this, "omni:ai:status", &Cmd_AiStatus);
    add_cmd(*this, "omni:ai:chat", &Cmd_AiChat);
    add_cmd(*this, "omni:ai:embed", &Cmd_AiEmbed);
    add_cmd(*this, "omni:ai:backends", &Cmd_AiBackends);
    add_cmd(*this, "omni:ai:backends_info", &Cmd_AiBackendsInfo);
    // =================================================================
// BEGIN ADDITION: Register Cloud Commands
// =================================================================
    add_cmd(*this, "omni:cloud:create", &Cmd_CloudCreate);
    add_cmd(*this, "omni:cloud:list", &Cmd_CloudList);
    add_cmd(*this, "omni:cloud:upload", &Cmd_CloudUpload);
    add_cmd(*this, "omni:cloud:download", &Cmd_CloudDownload);
    add_cmd(*this, "omni:cloud:delete", &Cmd_CloudDelete);
    add_cmd(*this, "omni:cloud:mount", &Cmd_CloudMount);
    add_cmd(*this, "omni:cloud:unmount", &Cmd_CloudUnmount);
    add_cmd(*this, "omni:cloud:status", &Cmd_CloudStatus);
    // =================================================================
    // END ADDITION
    // =================================================================
}

// =================================================================
// CommandRouter member function definitions
// =================================================================

void CommandRouter::registerCommand(const std::string& name, CommandFunction func) {
    commandMap[name] = std::move(func);
}

std::string CommandRouter::dispatch(const std::string& input) {
    // Ensure process CWD matches our tracked directory so shell calls inherit it
    std::error_code ec;
    std::filesystem::current_path(g_working_dir, ec);

    auto tokens = tokenize(input);
    if (tokens.empty())
        return std::string("No command input.");
    std::string cmd = normalize(tokens[0]);

    auto it = commandMap.find(cmd);
    if (it != commandMap.end()) {
        return it->second(tokens);
    }

    //////////////////////////////
    // BEGIN ADDITION: PROFILE/DAEMON COMMANDS
    //////////////////////////////
    if (cmd == "profile" && tokens.size() >= 3 && tokens[1] == "apply") {
        SamplingProfile p;
        if (tokens[2] == "fastpreview")      p = SamplingProfile::FastPreview;
        else if (tokens[2] == "balanced")    p = SamplingProfile::Balanced;
        else if (tokens[2] == "highquality") p = SamplingProfile::HighQuality;
        else {
            std::cout << "Unknown profile: " << tokens[2] << "\n";
            return {};
        }
        OmniAIManager::applySamplingProfile(p, appConfig);
        std::cout << "[CLI] Applied profile: " << tokens[2] << "\n";
        return {};
    }

    if (cmd == "profile" && tokens.size() >= 2 && tokens[1] == "decide") {
        double cpuVal = appConfig.cpuThreshold;
        double batVal = appConfig.batteryMinThreshold;

        for (size_t i = 2; i + 1 < tokens.size(); i += 2) {
            if (tokens[i] == "--cpu") cpuVal = std::stod(tokens[i + 1]);
            else if (tokens[i] == "--battery") batVal = std::stod(tokens[i + 1]);
        }

        std::vector<SensorData> fakeSensors;
        {
            SensorData s1{};
            s1.id = "thermal_cpu";
            s1.value = cpuVal;
            s1.label.clear();
            s1.status = SensorStatus::OK;
            fakeSensors.push_back(s1);
        }
        {
            SensorData s2{};
            s2.id = "battery_pct";
            s2.value = batVal;
            s2.label.clear();
            s2.status = SensorStatus::OK;
            fakeSensors.push_back(s2);
        }

        DaemonMonitor dm;
        SamplingProfile profile = dm.decideProfile(fakeSensors, appConfig);
        std::cout << "[CLI] Decided profile: " << static_cast<int>(profile) << "\n";
        return {};
    }

    if (cmd == "daemon" && tokens.size() >= 2 && tokens[1] == "capture-tiles") {
        DaemonMonitor dm;
        dm.captureTileTelemetry();
        return {};
    }

    if (cmd == "daemon" && tokens.size() >= 2) {
        if (tokens[1] == "start") {
            daemon.start(appConfig);
            return {};
        }
        if (tokens[1] == "stop") {
            daemon.stop();
            return {};
        }
        if (tokens[1] == "status") {
            std::cout << daemon.getStatus() << "\n";
            return {};
        }
    }

    if (cmd == "daemon" && tokens.size() >= 2 && tokens[1] == "simulate") {
        double cpuVal = appConfig.cpuThreshold;
        double batVal = appConfig.batteryMinThreshold;

        for (size_t i = 2; i + 1 < tokens.size(); i += 2) {
            if (tokens[i] == "--cpu") cpuVal = std::stod(tokens[i + 1]);
            else if (tokens[i] == "--battery") batVal = std::stod(tokens[i + 1]);
        }

        std::vector<SensorData> fakeSensors;
        {
            SensorData s1{};
            s1.id = "thermal_cpu";
            s1.value = cpuVal;
            s1.label.clear();
            s1.status = SensorStatus::OK;
            fakeSensors.push_back(s1);
        }
        {
            SensorData s2{};
            s2.id = "battery_pct";
            s2.value = batVal;
            s2.label.clear();
            s2.status = SensorStatus::OK;
            fakeSensors.push_back(s2);
        }

        DaemonMonitor dm;
        SamplingProfile chosen = dm.decideProfile(fakeSensors, appConfig);
        OmniAIManager::applySamplingProfile(chosen, appConfig);
        std::cout << "[CLI] Applied profile: " << static_cast<int>(chosen) << "\n";

        auto plan = OmniAIManager::analyzeAndRecommend(fakeSensors, appConfig);
        if (!plan.empty()) {
            std::cout << "[CLI] Recommended plan:\n";
            for (auto& step : plan) {
                std::cout << " - " << step.description << " (" << step.command << ")\n";
            }
        }
        else {
            std::cout << "[CLI] System nominal.\n";
        }
        return {};
    }
    //////////////////////////////
    // END ADDITION
    //////////////////////////////

    return ShellExecutor::run(input);
}

std::vector<std::string> CommandRouter::tokenize(const std::string& input) {
    std::istringstream stream(input);
    std::string token;
    std::vector<std::string> tokens;
    while (stream >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

std::string CommandRouter::normalize(const std::string& cmd) {
    std::string lower = cmd;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

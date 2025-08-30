// =================================================================
// main.cpp
// UPDATED with administrative privilege check and correct OmniConfigNS::load usage.
// ADDED: LLM engine integration (no external dependencies)
// PATCHED: Quiet-mode + UTF-8 handling; LLM generation respects redirection (no streaming/prompts when quiet)
// NEW: Added programmatic self-exclusion for Windows Defender.
// =================================================================
#include <iostream>
#include <string>
#include <filesystem>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <cstdio>

#include "ShellExecutor.h"
#include "OmniAIManager.h"
#include "SensorManager.h"
#include "TileAnalytics.h"
#include "OmniConfig.h"
#include "CommandRouter.h"
#include "ScriptRunner.h"
// #include "SensorManager.h" // FIX: Removed redundant include
#include "PolyglotC.h"
#include "JobManager.h"

// ADDED: bring in the no-dependency LLM engine you added to the project
#include "model.h"

// ADDED: bring in PMU monitor integration
#include "PMU.h"
#include <thread>
#include <atomic>
#include <chrono>

// ADDED: To call the new Defender exclusion function
#include "BinaryManip.h"

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#elif __linux__
#include <unistd.h> // For geteuid(), isatty()
#endif

// Global config object
ConfigState appConfig;

#ifdef _WIN32
// FIX: Marked function as static per warning VCR003
// Function to check if the process is running as an administrator
static bool isRunningAsAdmin() {
    BOOL fIsAdmin = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        // FIX: Initialize local variable per warning lnt-uninitialized-local
        TOKEN_ELEVATION Elevation = { 0 };
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        if (GetTokenInformation(hToken, TokenElevation, &Elevation, sizeof(Elevation), &cbSize)) {
            fIsAdmin = Elevation.TokenIsElevated;
        }
    }
    if (hToken) {
        CloseHandle(hToken);
    }
    return fIsAdmin;
}
#endif

// FIX: Marked function as static per warning VCR003
static void printBanner() {
    std::cout << R"(                                  ____  _                            
  ___   ___     ___   _        _ /  __|| |_   
 / _ \ | _ \   /   | |  \   ||(_)\__  \|  _ \ 
| (_) || |\ \_/ /| | | |\\  ||| | __) || | | | ?
 \___/ |_| \___/ |_| |_| \\_|||_||___/ |_| |_|(_)
         OmniShell RootMode CLI v1.0
    Multi-Syntax | Polyglot | Self-Healing
================================================
)" << std::endl;
}

// FIX: Marked function as static per warning VCR003
static void loadConfig() {
    // Attempt to load OmniConfig.xml into appConfig
    if (!OmniConfigNS::load("OmniConfig.xml", appConfig)) {
        std::cerr << "[*] Failed to load OmniConfig.xml, using default settings.\n";
    }
    else {
        std::cout << "[*] OmniConfig.xml loaded successfully.\n";
    }
}

// FIX: Marked function as static per warning VCR003
static void monitorSensors(CommandRouter& router) { // Pass router by reference
    if (!appConfig.monitorSensors) {
        std::cout << "[*] Sensor monitoring is disabled in OmniConfig.xml." << std::endl;
        return;
    }
    std::cout << "[*] Monitoring active sensors...\n";
    auto sensors = SensorManager::listSensors();
    for (const auto& s : sensors) {
        std::cout << "[" << s.label << "] " << s.value << " " << s.unit
            << " (Status: " << static_cast<int>(s.status) << ")\n";
    }

    auto plan = OmniAIManager::analyzeAndRecommend(sensors, appConfig);

    if (!plan.empty()) {
        std::cout << "[!] AI has recommended a repair plan. Executing...\n";
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
                result = router.dispatch(step.command); // Use the passed router
                break;
            case ShellType::BASH:
                result = ShellExecutor::run(step.command);
                break;
            }
            std::cout << "    Result:\n" << result << std::endl;
        }
    }
    else {
        std::cout << "[+] AI Analysis: System nominal." << std::endl;
    }
}

// =================================================================
// ADDED: PMU CLI handler (non-destructive integration)
// =================================================================
// FIX: Marked function as static per warning VCR003
static int omni_pmu_main(int argc, char* argv[]) {
    using namespace std::chrono;

    int intervalMs = 1000;
    int topN = 5;
    std::atomic_bool stopFlag{ false };

    // Minimal arg parse for: --interval/-i <ms>, --top/-t <N>
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--interval" || arg == "-i") && i + 1 < argc) {
            intervalMs = std::stoi(argv[++i]);
        }
        else if ((arg == "--top" || arg == "-t") && i + 1 < argc) {
            topN = std::stoi(argv[++i]);
        }
    }

    // Respect quiet mode bannering
    bool quiet = false;
#ifdef _WIN32
    {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, "OMNI_QUIET") == 0 && value != nullptr) {
            quiet = (value[0] == '1');
            free(value);
        }
    }
#else
    {
        const char* q = std::getenv("OMNI_QUIET");
        quiet = (q && q[0] == '1');
    }
#endif

    auto handler = [](const std::string& summary) {
        std::cout << summary << std::endl;
        };

    if (!quiet) {
        std::cout << "[PMU] Monitoring every " << intervalMs
            << " ms, top " << topN << " threads\n";
        std::cout << "[PMU] Press Ctrl+C to stop...\n";
    }

    PMU::MonitorSelf(milliseconds(intervalMs), topN, handler, &stopFlag);
    return 0;
}

// =================================================================
// ADDED: LLM engine integration (no external dependencies)
// - Standalone CLI flags: --llm-run, --llm-repl
// - Interactive OmniShell inline commands: llm:help, llm:load, llm:gen, llm:status
// PATCHED: Respect OMNI_QUIET for non-streaming, no prompts when redirected
// =================================================================
namespace LLM {

    static CLLF g_engine;
    static bool g_loaded = false;
    static std::string g_modelPath;

    struct Options {
        std::string model;
        std::string prompt = "Hello";
        int n_predict = 64;
        float temp = 0.8f;
        int top_k = 40;
        float top_p = 0.95f;
        bool stream = true;
        bool repl = false;
        bool run = false;
    };

    // PATCHED: helper to detect quiet mode from environment (safe CRT version)
    static bool isQuiet() {
        char* value = nullptr;
        size_t len = 0;
        bool quiet = false;

#ifdef _WIN32
        if (_dupenv_s(&value, &len, "OMNI_QUIET") == 0 && value != nullptr) {
            quiet = (value[0] == '1');
            free(value); // must free the string when done
        }
#else
        const char* q = std::getenv("OMNI_QUIET"); // POSIX getenv is fine
        quiet = (q && q[0] == '1');
#endif

        return quiet;
    }

    static void printHelp() {
        std::cout <<
            R"(LLM usage:

  Standalone:
    --llm-run --llm-model <path> [--llm-prompt "<text>"] [--llm-n <N>]
              [--llm-temp <T>] [--llm-top-k <K>] [--llm-top-p <P>] [--llm-stream]
    --llm-repl --llm-model <path>

  Inline commands inside OmniShell interactive loop:
    llm:help
    llm:load <path>
    llm:status
    llm:gen "<text>" [--n N] [--temp T] [--top-k K] [--top-p P] [--nostream]

Notes:
  - Model is a .cllf file compatible with the zero-dependency engine.
  - If --llm-stream is given, tokens stream to stdout.
)" << std::endl;
    }

    static bool ensureLoaded(const std::string& path) {
        if (g_loaded && (path.empty() || path == g_modelPath)) return true;
        if (path.empty()) {
            std::cerr << "[LLM] No model loaded. Use llm:load <path> or pass --llm-model.\n";
            return false;
        }
        CLLF eng;
        if (!eng.load(path)) {
            std::cerr << "[LLM] Failed to load model: " << path << "\n";
            return false;
        }
        g_engine = std::move(eng);
        g_loaded = true;
        g_modelPath = path;
        std::cout << "[LLM] Model loaded: " << g_modelPath << "\n";
        return true;
    }

    // PATCHED: honor quiet mode by disabling streaming and prompts
    static int runOnce(Options opt) {
        if (!ensureLoaded(opt.model)) return 2;

        const bool quiet = isQuiet();
        if (quiet) {
            opt.stream = false;
        }

        if (opt.stream && !quiet) {
            std::cout << ">> ";
        }

        // generate
        std::string out = g_engine.generate(opt.prompt, opt.n_predict, opt.temp, opt.top_k, opt.top_p, opt.stream);

        // when not streaming (either by flag or quiet), emit the full text once
        if (!opt.stream) {
            std::cout << out << "\n";
        }
        return 0;
    }

    static void repl() {
        if (!g_loaded) {
            std::cerr << "[LLM] No model loaded. Use --llm-model <path>.\n";
            return;
        }
        std::cout << "[LLM] REPL: enter 'exit' to quit. Use: gen <text> [--n N --temp T --top-k K --top-p P --nostream]\n";
        std::string line;
        while (true) {
            std::cout << "[llm] >>> ";
            if (!std::getline(std::cin, line)) break;
            if (line == "exit") break;
            if (line.rfind("load ", 0) == 0) {
                std::string p = line.substr(5);
                if (ensureLoaded(p)) continue;
                else continue;
            }
            if (line.rfind("gen ", 0) == 0) {
                // naive parse: gen <text> [flags]
                Options o;
                o.model = g_modelPath;
                o.stream = !isQuiet(); // PATCHED: default stream only when interactive

                // extract quoted or rest-of-line as prompt first
                std::string rest = line.substr(4);
                // If quoted
                std::string prompt = rest;
                size_t first = rest.find('"');
                if (first != std::string::npos) {
                    size_t second = rest.find('"', first + 1);
                    if (second != std::string::npos) {
                        prompt = rest.substr(first + 1, second - first - 1);
                        rest = rest.substr(second + 1);
                    }
                }
                else {
                    // take first token till flags start
                    size_t pos = rest.find(" --");
                    if (pos != std::string::npos) {
                        prompt = rest.substr(0, pos);
                        rest = rest.substr(pos);
                    }
                    else {
                        prompt = rest;
                        rest.clear();
                    }
                }
                // trim
                auto trim = [](std::string s) {
                    size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return std::string{};
                    size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
                    };
                o.prompt = trim(prompt);

                // parse flags in rest
                std::istringstream is(rest);
                std::string tok;
                while (is >> tok) {
                    if (tok == "--n") { int v; if (is >> v) o.n_predict = v; }
                    else if (tok == "--temp") { float v; if (is >> v) o.temp = v; }
                    else if (tok == "--top-k") { int v; if (is >> v) o.top_k = v; }
                    else if (tok == "--top-p") { float v; if (is >> v) o.top_p = v; }
                    else if (tok == "--nostream") { o.stream = false; }
                }
                (void)runOnce(o);
                continue;
            }
            if (line == "status" || line == "llm:status") {
                if (!g_loaded) std::cout << "[LLM] No model loaded.\n";
                else {
                    const auto& cfg = g_engine.W.cfg;
                    std::cout << "[LLM] Model: " << g_modelPath << "\n"
                        << "       vocab=" << cfg.vocab_size
                        << " d_model=" << cfg.d_model
                        << " layers=" << cfg.n_layers
                        << " heads=" << cfg.n_heads
                        << " max_seq=" << cfg.max_seq << "\n";
                }
                continue;
            }
            std::cout << "[LLM] Unknown: " << line << "\n";
        }
    }

    // Parse LLM CLI flags out-of-band without disturbing existing modes
    static bool parseArgs(int argc, char** argv, Options& out) {
        // If none of the LLM flags are present, return false
        bool seen = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--llm", 0) == 0) { seen = true; break; }
        }
        if (!seen) return false;

        // Defaults
        out = Options{};
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char* name)->std::string {
                if (i + 1 >= argc) {
                    std::cerr << "[LLM] Missing value for " << name << "\n"; std::exit(2);
                }
                return std::string(argv[++i]);
                };
            if (a == "--llm-model") out.model = need("--llm-model").c_str();
            else if (a == "--llm-prompt") out.prompt = need("--llm-prompt").c_str();
            else if (a == "--llm-n") out.n_predict = std::stoi(need("--llm-n"));
            else if (a == "--llm-temp") out.temp = std::stof(need("--llm-temp"));
            else if (a == "--llm-top-k") out.top_k = std::stoi(need("--llm-top-k"));
            else if (a == "--llm-top-p") out.top_p = std::stof(need("--llm-top-p"));
            else if (a == "--llm-stream") out.stream = true;
            else if (a == "--llm-run") out.run = true;
            else if (a == "--llm-repl") out.repl = true;
            else if (a == "--llm-help") { printHelp(); std::exit(0); }
        }
        if (!out.run && !out.repl) {
            // If any LLM flags but no action, default to run
            out.run = true;
        }
        if (out.model.empty()) {
            std::cerr << "[LLM] --llm-model <path> is required.\n";
            printHelp();
            std::exit(2);
        }
        return true;
    }

    // Inline OmniShell interactive commands
    static bool handleInline(const std::string& input) {
        if (input == "llm:help") { printHelp(); return true; }
        if (input.rfind("llm:load ", 0) == 0) {
            std::string path = input.substr(9);
            return ensureLoaded(path);
        }
        if (input == "llm:status") {
            if (!g_loaded) std::cout << "[LLM] No model loaded.\n";
            else {
                const auto& cfg = g_engine.W.cfg;
                std::cout << "[LLM] Model: " << g_modelPath << "\n"
                    << "       vocab=" << cfg.vocab_size
                    << " d_model=" << cfg.d_model
                    << " layers=" << cfg.n_layers
                    << " heads=" << cfg.n_heads
                    << " max_seq=" << cfg.max_seq << "\n";
            }
            return true;
        }
        if (input.rfind("llm:gen", 0) == 0) {
            // Form: llm:gen "<text>" [--n N] [--temp T] [--top-k K] [--top-p P] [--nostream]
            Options o;
            o.model = g_modelPath;
            if (!g_loaded) { std::cerr << "[LLM] No model loaded. Use llm:load <path>.\n"; return true; }
            std::string rest = input.substr(7);
            // Parse quoted prompt
            std::string prompt = rest;
            size_t first = rest.find('"');
            if (first != std::string::npos) {
                size_t second = rest.find('"', first + 1);
                if (second != std::string::npos) {
                    prompt = rest.substr(first + 1, second - first - 1);
                    rest = rest.substr(second + 1);
                }
            }
            else {
                size_t pos = rest.find(" --");
                if (pos != std::string::npos) {
                    prompt = rest.substr(0, pos);
                    rest = rest.substr(pos);
                }
                else { prompt = rest; rest.clear(); }
            }
            auto trim = [](std::string s) {
                size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return std::string{};
                size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
                };
            o.prompt = trim(prompt);
            o.stream = !isQuiet(); // PATCHED: default to non-stream when redirected

            // Flags
            std::istringstream is(rest);
            std::string tok;
            while (is >> tok) {
                if (tok == "--n") { int v; if (is >> v) o.n_predict = v; }
                else if (tok == "--temp") { float v; if (is >> v) o.temp = v; }
                else if (tok == "--top-k") { int v; if (is >> v) o.top_k = v; }
                else if (tok == "--top-p") { float v; if (is >> v) o.top_p = v; }
                else if (tok == "--nostream") { o.stream = false; }
            }
            (void)runOnce(o);
            return true;
        }
        return false;
    }

} // namespace LLM
// =================================================================

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Check for admin privileges and relaunch if necessary
    if (!isRunningAsAdmin()) {
        char szPath[MAX_PATH];
        if (GetModuleFileNameA(NULL, szPath, ARRAYSIZE(szPath))) {
            SHELLEXECUTEINFOA sei = { sizeof(sei) };
            sei.lpVerb = "runas";
            sei.lpFile = szPath;
            sei.hwnd = NULL;
            sei.nShow = SW_NORMAL;
            if (ShellExecuteExA(&sei)) {
                return 0;
            }
        }
        std::cerr << "[Error] Administrative privileges are required. Please re-run as administrator." << std::endl;
        return 1;
    }
    // Quiet/TTY detection + UTF-8 console configuration
    bool quietMode = false;
    {
        DWORD type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
        quietMode = (type != FILE_TYPE_CHAR);
        if (!quietMode) {
            SetConsoleOutputCP(CP_UTF8);
            SetConsoleCP(CP_UTF8);
        }
        else {
            // file/pipe: use full buffering to avoid interleaved partial writes
            setvbuf(stdout, nullptr, _IOFBF, 0);
        }
        // Export for downstream modules (optional, non-fatal)
        SetEnvironmentVariableA("OMNI_QUIET", quietMode ? "1" : "0");
    }
#elif __linux__
    // On Linux, check for root privileges and warn the user.
    if (geteuid() != 0) {
        std::cout << "[Warning] Not running as root. Some commands may require sudo." << std::endl;
    }
    // Quiet/TTY detection for POSIX
    bool quietMode = !isatty(STDOUT_FILENO);
    if (quietMode) {
        setvbuf(stdout, nullptr, _IOFBF, 0);
    }
    setenv("OMNI_QUIET", quietMode ? "1" : "0", 1);
#endif

    if (!quietMode) {
        printBanner();
    }
    loadConfig(); // <-- uses the two-argument OmniConfigNS::load
    JobManager::Initialize(); // Initialize the job manager

    // =================================================================
    // Early LLM CLI handling that does not interfere with existing modes
    // If LLM flags are present, handle and exit; otherwise continue normal flow.
    // Force non-streaming when stdout is redirected (quiet mode).
    // =================================================================
    {
        LLM::Options opt;
        if (LLM::parseArgs(argc, argv, opt)) {
            if (quietMode) {
                opt.stream = false;
            }
            if (!LLM::ensureLoaded(opt.model)) { JobManager::Shutdown(); return 2; }
            if (opt.run) { int rc = LLM::runOnce(opt); JobManager::Shutdown(); return rc; }
            if (opt.repl) {
                if (quietMode) {
                    std::cerr << "[LLM] REPL not suitable when output is redirected.\n";
                    JobManager::Shutdown();
                    return 1;
                }
                LLM::repl();
                JobManager::Shutdown();
                return 0;
            }
        }
    }

    CommandRouter router;

    if (argc > 1) {
        std::string mode = argv[1];
        if (mode == "--script" && argc > 2) {
            std::cout << ScriptRunner::runScript(argv[2]) << "\n";
            return 0;
        }
        else if (mode == "--build" && argc > 2) {
            std::cout << PolyglotC::buildFromXml(argv[2]) << "\n";
            return 0;
        }
        else if (mode == "--monitor") {
            monitorSensors(router); // Pass the router object
            return 0;
        }
        // ADDED: --pmu mode wiring (non-destructive)
        else if (mode == "--pmu") {
            // Pass through remaining args to pmu handler
            return omni_pmu_main(argc - 1, argv + 1);
        }
        else {
            std::cerr << "Usage:\n"
                "  OmniShell.exe --script <file.scabi>\n"
                "  OmniShell.exe --build <file.xml>\n"
                "  OmniShell.exe --monitor\n"
                "  OmniShell.exe --pmu [--interval <ms>] [--top <N>]\n"
                "  OmniShell.exe --llm-run --llm-model <path> [--llm-prompt \"...\"] [--llm-n N] [--llm-temp T] [--llm-top-k K] [--llm-top-p P] [--llm-stream]\n"
                "  OmniShell.exe --llm-repl --llm-model <path>\n";
            return 1;
        }
    }

    // Interactive mode
    std::string input;
    if (!quietMode) {
        std::cout << "[OmniShell] Enter 'exit' to quit.\n";
        std::cout << "[OmniShell] Tip: try 'llm:help' to use the local LLM engine.\n"; // ADDED
    }
    while (true) {
        JobManager::checkJobs(); // Check for completed background jobs
        if (!quietMode) {
            std::cout << std::filesystem::current_path().string() << " >>> ";
        }
        if (!std::getline(std::cin, input)) break;
        if (input == "exit") break;
        if (input.empty()) continue;

        // intercept inline LLM commands (does not touch CommandRouter)
        if (LLM::handleInline(input)) {
            continue;
        }

        bool isBackground = false;
        if (!input.empty() && input.back() == '&') {
            isBackground = true;
            input.pop_back();
            // Trim whitespace
            size_t endpos = input.find_last_not_of(" \t");
            if (std::string::npos != endpos) {
                input = input.substr(0, endpos + 1);
            }
        }

        if (isBackground) {
            std::string commandToRun = input;
            int jobId = JobManager::addJob(commandToRun, [&router, commandToRun]() {
                return router.dispatch(commandToRun);
                });
            if (!quietMode) {
                std::cout << "[" << jobId << "]" << std::endl;
            }
        }
        else {
            std::string output = router.dispatch(input);
            if (!output.empty()) {
                std::cout << output << "\n";
            }
        }
    }

    JobManager::Shutdown(); // Shutdown the job manager
    return 0;
}

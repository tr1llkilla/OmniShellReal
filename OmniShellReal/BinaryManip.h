Copyright © 2025 Cadell Richard Anderson
//BinaryManip.h
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <cstdint>
#include "types.h"

namespace BinaryManip {

    // Portable process identifier alias to avoid pid_t on Windows
#if defined(_WIN32)
    using ProcId = unsigned long;
    // DWORD PID
#else
    using ProcId = int;
    // pid_t-compatible stand-in (keeps header light)
#endif

    // -------------------- Taxonomy --------------------

    enum class Arch {
        Unknown, X86, X64, ARM, ARM64, RISCV64, PPC, MIPS
    };
    enum class OS {
        Unknown, Windows, Linux, Mac
    };
    enum class ManipKind {
        Translate,   // ISA-to-ISA translation
        Rewrite,     // same-ISA insert/patch
        Interpret,   // step-by-step execute
        Emulate,     // machine/system emulation
        VirtAssist   // virtualization-sensitive rewriting
    };
    enum class Mode {
        Static,   // on-disk transform
        Dynamic   // in-memory, runtime
    };
    // -------------------- Descriptors --------------------

    struct BinaryInfo {
        std::string path;
        Arch arch = Arch::Unknown;
        OS os = OS::Unknown;
        bool isLibrary = false;
        bool positionIndependent = false;
        bool stripped = false;
        // no symbols
        uint64_t imageBase = 0;
        uint64_t entryRVA = 0;
    };
    struct Instrumentation {
        bool profile = false;
        // timing, edge counts
        bool memChecks = false;
        // OOB/NP checks
        bool syscallLog = false;
        // trace syscalls
        bool sandbox = false;
        // restrict syscalls
        bool coverage = false;
        // block/edge coverage
        bool taint = false;
        // optional heavy
    };

    struct TranslateOpts {
        Arch targetArch = Arch::Unknown;
        Mode mode = Mode::Static;       // static or dynamic translation
        bool cacheBlocks = true;
        // for dynamic mode
        bool preserveSymbols = true;
        Instrumentation inst;
    };
    struct RewriteOpts {
        Mode mode = Mode::Static;
        // static or dynamic rewriting
        Instrumentation inst;
        bool inlinePatch = true;
        // try inline, else trampoline
        bool attachToRunning = false;
        // dynamic attach
    };

    struct InterpretOpts {
        bool collectTrace = false;
        // instruction trace
        bool liftHotPaths = true;
        // hybrid: lift to native JIT
        Instrumentation inst;
    };
    struct EmulateOpts {
        bool fullSystem = false;
        // SoC/peripherals
        bool hwAssistWhenPossible = true;
        // VT-x/ARMv8 virtualization
        Instrumentation inst;
    };
    struct VirtAssistOpts {
        bool rewriteSensitive = true;
        // modify privileged ops
        bool shadowPageTables = true;
        bool inlineMonitor = true;
        // inline checks via trampolines
        Instrumentation inst;
    };
    struct Result {
        bool ok = false;
        std::string message;
        std::optional<std::string> outputPath;
        // for static transforms
    };

    // ADDED: New struct for AI analysis results
    struct AIAnalysisResult {
        bool success = false;
        std::string message;
        std::vector<std::string> findings; // e.g., "Obfuscation pattern detected at 0x401000"
        float confidence = 0.0f;
    };


    // -------------------- Logging --------------------

    using LogFn = std::function<void(const std::string&)>;
    void SetLogger(LogFn);

    // -------------------- Core API --------------------

    std::optional<BinaryInfo> Probe(const std::string& path);
    Result Translate(const std::string& inputPath, const TranslateOpts& opts);
    Result Rewrite(const std::string& inputPath, const RewriteOpts& opts);
    Result Interpret(const std::string& inputPath, const InterpretOpts& opts);
    Result Emulate(const std::string& inputPath, const EmulateOpts& opts);
    Result VirtAssist(const std::string& inputPath, const VirtAssistOpts& opts);

    // -------------------- Analysis helpers --------------------

    std::vector<std::string> DiscoverSymbols(const std::string& path);
    std::vector<std::string> ListSections(const std::string& path);
    std::vector<std::string> QuickCFG(const std::string& path); // coarse basic-block map

    // ADDED: New advanced analysis function declarations
    std::optional<size_t> FindFirstDifference(const std::string& file1_path, const std::string& file2_path);
    AIAnalysisResult AnalyzeWithAI(const std::string& path);


    // -------------------- Runtime attach (portable) --------------------

    Result AttachAndInstrument(ProcId processId, const RewriteOpts& opts);

    // -------------------- Capability discovery --------------------

    bool SupportsDynAttach();
    bool SupportsInlinePatch();
} // namespace BinaryManip

// =================================================================
// OmniEditorIDE.cpp
// =================================================================
#if defined(_WIN32)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#endif
#include "BinaryManip.h"
#include "OmniEditorIDE.h"
#include "ShellExecutor.h"

#include <iostream>
#include <sstream>
#include <vector>
#include <fstream>
#include <string>
#include <filesystem>
#include <limits>
#include <cctype>
#include <optional>
#include <cstdlib>
#include <unordered_map>
#include <thread>
#include <algorithm>
#include <new>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

namespace OmniEditorIDE {

    // In-memory buffers: pair<filename, contents>
    static std::vector<std::pair<std::string, std::string>> buffers;
    static size_t activeBuffer = 0;

    // Session preferences
    static bool autoSaveAfterZ = false; // Off by default

    // Forward declarations
    static void RenderActiveBufferPaged();                           // wrapper: 0-arg
    static void RenderActiveBufferPaged(size_t maxLines);            // wrapper: 1-arg
    static void RenderActiveBufferPagedImpl(size_t startLine, size_t maxLines, size_t spacing); // core impl

    static void RenderActiveBufferHeader(bool modified);
    static void SwitchToBuffer(size_t index);
    static void SearchInBuffer(const std::string& query);
    static void Highlight(const std::string& token);
    static void CompileCurrentBuffer();
    static void ExportBufferToFile(size_t bufferId, const std::string& filepath);
    static std::string PromptLine(const char* prompt);
    static size_t CountLines(const std::string& s);

    // Editing helpers
    static std::vector<std::string> SplitLines(const std::string& s);
    static std::string JoinLines(const std::vector<std::string>& lines);
    static void SetActiveBufferText(const std::string& text, bool markModified = true);
    static bool isModified = false;

    // ========== Adaptive paging: config, detection, and decisions ==========

    struct AdaptiveDefaults {
        size_t defaultCap = 200;        // baseline if nothing else applies
        size_t constrainedCap = 100;    // low RAM / few cores
        size_t veryConstrainedCap = 50; // very low resources
        size_t minRowsReserved = 5;     // rows kept for status/prompts
        size_t hardCap = 1000;          // absolute upper bound for safety
        size_t defaultSpacing = 1;      // 1=dense; >1 inserts blank lines
        bool diagnostics = true;        // show one-line banner and hints
        bool adaptiveEnabled = true;    // enable hardware-based caps
        bool sshAware = true;           // respect terminal rows (esp. SSH)
    };

    struct AdaptiveConfig {
        bool adaptiveEnabled = true;
        bool sshAware = true;
        bool diagnostics = true;
        size_t requestedMaxLines = 0; // 0 means unspecified
        size_t minRowsReserved = 5;
        size_t hardCap = 1000;
        size_t defaultSpacing = 1;
        std::string sourceRequestedMaxLines = "auto";
    };

    struct TerminalInfo {
        int rows = 0;
        int cols = 0;
        bool viaSSH = false;
        std::string sshTty;
    };

    struct SystemInfo {
        uint64_t totalMemMB = 0;
        unsigned cores = 0;
        bool constrained = false;
        bool veryConstrained = false;
    };

    struct PageDecision {
        size_t cap = 0;            // final page cap
        size_t reservedRows = 0;   // rows kept for UI
        std::string reason;        // summary for diagnostics
    };

    // Global adaptive state (initialized in LaunchInteractiveUI)
    static AdaptiveDefaults gDfl{};
    static AdaptiveConfig gCfg{};
    static TerminalInfo gTerm{};
    static SystemInfo gSys{};
    static PageDecision gDec{};

    // ---------- Utilities ----------
    static inline std::optional<std::string> getenv_str(const char* k) {
        const char* v = std::getenv(k);
        if (!v || !*v) return std::nullopt;
        return std::string(v);
    }
    static inline std::optional<long long> getenv_ll(const char* k) {
        auto s = getenv_str(k);
        if (!s) return std::nullopt;
        try { return std::stoll(*s); }
        catch (...) { return std::nullopt; }
    }
    static inline std::optional<bool> getenv_bool(const char* k) {
        auto s = getenv_str(k);
        if (!s) return std::nullopt;
        std::string v = *s;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
        if (v == "0" || v == "false" || v == "no" || v == "off") return false;
        return std::nullopt;
    }

    // Optional: load simple key=value props from file specified by OMNI_EDITOR_PROPS
    static std::unordered_map<std::string, std::string> LoadPropsFromEnvFile() {
        std::unordered_map<std::string, std::string> props;
        auto p = getenv_str("OMNI_EDITOR_PROPS");
        if (!p) return props;
        std::ifstream f(*p);
        if (!f) return props;
        std::string line;
        while (std::getline(f, line)) {
            // trim leading/trailing spaces
            auto l = line;
            auto isSpace = [](unsigned char c) { return std::isspace(c); };
            l.erase(l.begin(), std::find_if(l.begin(), l.end(), [&](char c) { return !isSpace((unsigned char)c); }));
            l.erase(std::find_if(l.rbegin(), l.rend(), [&](char c) { return !isSpace((unsigned char)c); }).base(), l.end());
            if (l.empty() || l[0] == '#') continue;
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = l.substr(0, eq);
            std::string v = l.substr(eq + 1);
            // trim key
            k.erase(k.begin(), std::find_if(k.begin(), k.end(), [&](char c) { return !isSpace((unsigned char)c); }));
            k.erase(std::find_if(k.rbegin(), k.rend(), [&](char c) { return !isSpace((unsigned char)c); }).base(), k.end());
            // trim value
            v.erase(v.begin(), std::find_if(v.begin(), v.end(), [&](char c) { return !isSpace((unsigned char)c); }));
            v.erase(std::find_if(v.rbegin(), v.rend(), [&](char c) { return !isSpace((unsigned char)c); }).base(), v.end());
            if (!k.empty()) props[k] = v;
        }
        return props;
    }

    // Terminal detection
    static TerminalInfo DetectTerminal() {
        TerminalInfo t;
#if defined(_WIN32)
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut && GetConsoleScreenBufferInfo(hOut, &csbi)) {
            t.cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            t.rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
#else
        struct winsize ws {};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            t.cols = ws.ws_col;
            t.rows = ws.ws_row;
        }
#endif
        // Fallback to env
        if (t.rows <= 0) {
            if (auto s = getenv_str("LINES")) {
                try { t.rows = (std::max)(0, std::stoi(*s)); }
                catch (...) {}
            }
        }
        if (t.cols <= 0) {
            if (auto s = getenv_str("COLUMNS")) {
                try { t.cols = (std::max)(0, std::stoi(*s)); }
                catch (...) {}
            }
        }
        // SSH hint
        if (getenv_str("SSH_TTY") || getenv_str("SSH_CONNECTION")) {
            t.viaSSH = true;
            if (auto s = getenv_str("SSH_TTY")) t.sshTty = *s;
        }
        return t;
    }
    // System detection
    static SystemInfo DetectSystem() {
        SystemInfo s{};
        s.cores = (std::max)(1u, std::thread::hardware_concurrency());
#if defined(_WIN32)
        MEMORYSTATUSEX msx{};
        msx.dwLength = sizeof(msx);
        if (GlobalMemoryStatusEx(&msx)) {
            s.totalMemMB = static_cast<uint64_t>(msx.ullTotalPhys / (1024ull * 1024ull));
        }
#else
        // Linux: /proc/meminfo; macOS/BSD may leave totalMemMB=0
        std::ifstream f("/proc/meminfo");
        if (f) {
            std::string k, unit;
            long long kb = 0;
            while (f >> k >> kb >> unit) {
                if (k == "MemTotal:") { s.totalMemMB = static_cast<uint64_t>(kb / 1024); break; }
            }
        }
#endif
        s.veryConstrained = (s.totalMemMB > 0 && s.totalMemMB < 1024) || (s.cores == 1);
        s.constrained = s.veryConstrained || (s.totalMemMB > 0 && s.totalMemMB < 2048) || (s.cores <= 2);
        return s;
    }

    // Config merge: props file + env
    static AdaptiveConfig MakeAdaptiveConfig(const AdaptiveDefaults& dfl) {
        AdaptiveConfig cfg{};
        cfg.adaptiveEnabled = dfl.adaptiveEnabled;
        cfg.sshAware = dfl.sshAware;
        cfg.diagnostics = dfl.diagnostics;
        cfg.requestedMaxLines = 0;
        cfg.minRowsReserved = dfl.minRowsReserved;
        cfg.hardCap = dfl.hardCap;
        cfg.defaultSpacing = dfl.defaultSpacing;
        cfg.sourceRequestedMaxLines = "auto";

        auto props = LoadPropsFromEnvFile();
        auto getProp = [&](const char* key) -> std::optional<std::string> {
            auto it = props.find(key);
            if (it != props.end() && !it->second.empty()) return it->second;
            return std::nullopt;
            };

        // 1) Props file (optional)
        if (auto p = getProp("editor.adaptive")) {
            std::string v = *p; std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            cfg.adaptiveEnabled = (v == "1" || v == "true" || v == "yes" || v == "on");
        }
        if (auto p = getProp("editor.ssh_aware")) {
            std::string v = *p; std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            cfg.sshAware = (v == "1" || v == "true" || v == "yes" || v == "on");
        }
        if (auto p = getProp("diagnostics.enabled")) {
            std::string v = *p; std::transform(v.begin(), v.end(), v.begin(), ::tolower);
            cfg.diagnostics = (v == "1" || v == "true" || v == "yes" || v == "on");
        }
        if (auto p = getProp("viewport.max_lines")) {
            try { cfg.requestedMaxLines = std::stoul(*p); cfg.sourceRequestedMaxLines = "props"; }
            catch (...) {}
        }
        if (auto p = getProp("viewport.min_rows_reserved")) {
            try { cfg.minRowsReserved = std::stoul(*p); }
            catch (...) {}
        }
        if (auto p = getProp("paging.hard_cap")) {
            try { cfg.hardCap = std::stoul(*p); }
            catch (...) {}
        }
        if (auto p = getProp("paging.default_spacing")) {
            try { cfg.defaultSpacing = std::stoul(*p); }
            catch (...) {}
        }

        // 2) Env overrides
        if (auto v = getenv_bool("OMNI_ADAPTIVE")) cfg.adaptiveEnabled = *v;
        if (auto v = getenv_bool("OMNI_SSH_AWARE")) cfg.sshAware = *v;
        if (auto v = getenv_bool("OMNI_DIAGNOSTICS")) cfg.diagnostics = *v;
        if (auto v = getenv_ll("OMNI_VIEWPORT_MAX_LINES")) {
            if (*v > 0) { cfg.requestedMaxLines = static_cast<size_t>(*v); cfg.sourceRequestedMaxLines = "env"; }
        }
        if (auto v = getenv_ll("OMNI_MIN_ROWS_RESERVED")) {
            if (*v >= 0) cfg.minRowsReserved = static_cast<size_t>(*v);
        }
        if (auto v = getenv_ll("OMNI_PAGING_HARD_CAP")) {
            if (*v > 0) cfg.hardCap = static_cast<size_t>(*v);
        }
        if (auto v = getenv_ll("OMNI_DEFAULT_SPACING")) {
            if (*v > 0) cfg.defaultSpacing = static_cast<size_t>(*v);
        }

        return cfg;
    }

    static PageDecision DecidePageCap(const AdaptiveDefaults& dfl,
        const AdaptiveConfig& cfg,
        const TerminalInfo& term,
        const SystemInfo& sys) {
        PageDecision d{};
        d.reservedRows = cfg.minRowsReserved;

        size_t cap = cfg.requestedMaxLines ? cfg.requestedMaxLines : dfl.defaultCap;
        std::string reason;

        if (cfg.adaptiveEnabled) {
            if (sys.veryConstrained) {
                cap = dfl.veryConstrainedCap;
                reason += "very constrained hardware; ";
            }
            else if (sys.constrained) {
                cap = dfl.constrainedCap;
                reason += "constrained hardware; ";
            }
            else {
                reason += "normal hardware; ";
            }
        }
        else {
            reason += "adaptive disabled; ";
        }

        if (cfg.sshAware && term.rows > 0) {
            size_t fit = (term.rows > static_cast<int>(d.reservedRows))
                ? static_cast<size_t>(term.rows) - d.reservedRows
                : 1;
            cap = (std::min)(cap, fit);
            reason += "terminal-aware rows; ";
        }

        cap = (std::min)(cap, cfg.hardCap);
        cap = (std::max)((size_t)1, cap);

        d.cap = cap;
        if (reason.empty()) reason = "default";
        d.reason = reason;
        return d;
    }

    static void PrintDiagnosticsBanner() {
        if (!gCfg.diagnostics) return;
        std::string banner = "[Adaptive Paging] ";
        banner += "RAM=" + (gSys.totalMemMB ? std::to_string(gSys.totalMemMB) + " MB" : std::string("unknown"));
        banner += ", cores=" + std::to_string(gSys.cores);
        if (gTerm.cols > 0 && gTerm.rows > 0) {
            banner += ", term=" + std::to_string(gTerm.cols) + "x" + std::to_string(gTerm.rows);
        }
        if (gTerm.viaSSH && gCfg.sshAware) banner += " (SSH)";
        banner += " -> cap=" + std::to_string(gDec.cap);
        banner += " [" + gDec.reason + "]";
        if (gCfg.requestedMaxLines) {
            banner += " (requested " + std::to_string(gCfg.requestedMaxLines) + " via " + gCfg.sourceRequestedMaxLines + ")";
        }
        std::cout << banner << "\n";
        std::cout << "[Hints] reserved_rows=" << gDec.reservedRows
            << ", hard_cap=" << gCfg.hardCap
            << ", spacing=" << gCfg.defaultSpacing
            << ", adaptive=" << (gCfg.adaptiveEnabled ? "on" : "off")
            << ", ssh_aware=" << (gCfg.sshAware ? "on" : "off") << "\n";
    }

    // Utility
    static size_t CountLines(const std::string& s) {
        if (s.empty()) return 0;
        size_t n = 0;
        for (char c : s) if (c == '\n') ++n;
        if (s.back() != '\n') ++n;
        return n;
    }

    // Newline helpers
    enum class NewlineStyle { CRLF, LF, CR, Mixed, None };

    static NewlineStyle DetectNewlineStyle(const std::string& s) {
        bool hasCRLF = false, hasLF = false, hasCR = false;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\r') {
                if (i + 1 < s.size() && s[i + 1] == '\n') { hasCRLF = true; ++i; }
                else { hasCR = true; }
            }
            else if (s[i] == '\n') {
                hasLF = true;
            }
        }
        int modes = (hasCRLF ? 1 : 0) + (hasLF ? 1 : 0) + (hasCR ? 1 : 0);
        if (modes == 0) return NewlineStyle::None;
        if (modes > 1) return NewlineStyle::Mixed;
        if (hasCRLF) return NewlineStyle::CRLF;
        if (hasLF) return NewlineStyle::LF;
        return NewlineStyle::CR;
    }

    static const char* StyleName(NewlineStyle st) {
        switch (st) {
        case NewlineStyle::CRLF: return "CRLF";
        case NewlineStyle::LF:   return "LF";
        case NewlineStyle::CR:   return "CR";
        case NewlineStyle::Mixed:return "Mixed";
        case NewlineStyle::None: return "None";
        }
        return "Unknown";
    }

    static std::string NormalizeNewlines(const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i) {
            char c = in[i];
            if (c == '\r') {
                if (i + 1 < in.size() && in[i + 1] == '\n') { out.push_back('\n'); ++i; }
                else { out.push_back('\n'); }
            }
            else {
                out.push_back(c);
            }
        }
        return out;
    }

    // File I/O
    static bool ReadFileAll(const std::string& filename, std::string& out) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;
        std::ostringstream ss;
        ss << file.rdbuf();
        out = ss.str();
        return true;
    }

    static bool WriteFileAll(const std::string& filename, const std::string& content) {
        std::ofstream out(filename, std::ios::binary);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        return true;
    }

    // Small helper to persist current active buffer to disk
    static bool SaveActiveBufferToDisk() {
        if (buffers.empty()) return false;
        return WriteFileAll(buffers[activeBuffer].first, buffers[activeBuffer].second);
    }

    // Buffers
    void OpenBuffer(const std::string& name, const std::string& contents) {
        buffers.emplace_back(name, contents);
        activeBuffer = buffers.size() - 1;
        isModified = false;
    }

    // Public API
    void open(const std::string& filename) {
        std::string raw;
        if (!ReadFileAll(filename, raw)) {
            std::cerr << "[Error] Cannot open file: " << filename << std::endl;
            return;
        }
        auto style = DetectNewlineStyle(raw);
        std::string content = NormalizeNewlines(raw);
        OpenBuffer(filename, content);

        RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);

        size_t lines = CountLines(content);
        std::cout << "\n[Open] " << filename
            << " | bytes: " << raw.size()
            << " | newline: " << StyleName(style)
            << " | lines: " << lines << "\n";

        LaunchInteractiveUI();
    }

    // Legacy display path (kept for completeness)
    void displayUI(const std::string& filename, const std::vector<std::string>& lines, bool isDirty) {
        std::cout << "------ " << filename << " ------" << (isDirty ? " (Modified)" : "") << "\n";
        for (const auto& line : lines) {
            std::cout << line << "\n";
        }
    }

    void saveFile(const std::string& filename, const std::vector<std::string>& lines) {
        std::ofstream out(filename);
        if (!out) {
            std::cerr << "[Error] Could not write to " << filename << "\n";
            return;
        }
        for (const auto& line : lines) out << line << "\n";
        std::cout << "[+] File saved: " << filename << "\n";
    }

    // Rendering
    static void RenderActiveBufferHeader(bool modified) {
        // ANSI clear to top-left; comment if your console dislikes it and use cls instead.
        std::cout << "\x1B[2J\x1B[H";
        std::cout.flush();

        if (buffers.empty()) {
            std::cout << "[OmniEditor] (no buffer)"
                << (modified ? " (Modified)" : "") << "\n";
            std::cout << "------------------------------------------------------------\n";
            return;
        }
        std::cout << "[OmniEditor] " << buffers[activeBuffer].first
            << (modified ? " (Modified)" : "")
            << " [AutoSaveZ: " << (autoSaveAfterZ ? "On" : "Off") << "]\n";
        std::cout << "------------------------------------------------------------\n";
    }

    // Core implementation with full control
    static void RenderActiveBufferPagedImpl(size_t startLine, size_t maxLines, size_t spacing) {
        RenderActiveBufferHeader(isModified);
        if (buffers.empty()) {
            std::cout << "[Hint] Press O to open a file.\n";
            return;
        }

        const std::string& buf = buffers[activeBuffer].second;
        const size_t totalLines = CountLines(buf);

        // Decide defaults if not provided
        size_t cap = maxLines;
        if (cap == 0) {
            if (gDec.cap > 0) {
                cap = gDec.cap;
            }
            else {
                TerminalInfo t = DetectTerminal();
                size_t reserved = gCfg.minRowsReserved ? gCfg.minRowsReserved : gDfl.minRowsReserved;
                if (t.rows > 0 && t.rows > static_cast<int>(reserved)) cap = static_cast<size_t>(t.rows) - reserved;
                else cap = gDfl.defaultCap;
                cap = (std::max)((size_t)1, (std::min)(cap, gCfg.hardCap ? gCfg.hardCap : gDfl.hardCap));
            }
        }
        else if (cap == (std::numeric_limits<size_t>::max)()) {
            cap = totalLines;
        }
        cap = (std::max)((size_t)1, (std::min)(cap, gCfg.hardCap ? gCfg.hardCap : gDfl.hardCap));

        size_t useSpacing = (std::max)((size_t)1, spacing);

        std::istringstream iss(buf);
        std::string line;
        size_t ln = 1;
        while (ln - 1 < startLine && std::getline(iss, line)) {
            ++ln;
        }

        size_t shown = 0;
        while (shown < cap && std::getline(iss, line)) {
            std::cout << ln << ": " << line << "\n";
            for (size_t k = 1; k < useSpacing; ++k) std::cout << "\n";
            ++shown;
            ++ln;
        }

        if (totalLines > (startLine + shown)) {
            std::cout << "[...] (truncated view) Use V to view all or U for custom page.\n";
        }
    }
    // Wrapper: legacy no-arg call
    static void RenderActiveBufferPaged() {
        RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
    }

    // Wrapper: legacy 1-arg call
    static void RenderActiveBufferPaged(size_t maxLines) {
        RenderActiveBufferPagedImpl(0, maxLines, gCfg.defaultSpacing);
    }

    static void SwitchToBuffer(size_t index) {
        if (index < buffers.size()) {
            activeBuffer = index;
            RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
            std::cout << "\n[Switched to: " << buffers[activeBuffer].first << "]\n";
        }
    }

    // Search/Highlight
    static void SearchInBuffer(const std::string& query) {
        if (buffers.empty() || query.empty()) return;
        std::istringstream iss(buffers[activeBuffer].second);
        std::string line;
        int lineNumber = 1; bool any = false;
        while (std::getline(iss, line)) {
            if (line.find(query) != std::string::npos) {
                std::cout << lineNumber << ": " << line << "\n";
                any = true;
            }
            lineNumber++;
        }
        if (!any) std::cout << "[Search] No matches.\n";
    }

    static void Highlight(const std::string& token) {
        if (buffers.empty() || token.empty()) return;
        const std::string& content = buffers[activeBuffer].second;
        size_t pos = 0; size_t count = 0;
        while ((pos = content.find(token, pos)) != std::string::npos) {
            std::cout << "[Highlight] " << token << " at pos " << pos << "\n";
            pos += token.length();
            ++count;
        }
        if (count == 0) std::cout << "[Highlight] No occurrences.\n";
    }

    // Build/export
    static void CompileCurrentBuffer() {
        if (buffers.empty()) return;
        std::cout << "[Compiler] Compiling buffer: " << buffers[activeBuffer].first << "...\n";
        // Hook to your build system here via ShellExecutor if desired
        std::cout << "[Compiler] Compilation completed.\n";
    }

    static void ExportBufferToFile(size_t bufferId, const std::string& filepath) {
        if (bufferId >= buffers.size() || filepath.empty()) return;
        if (WriteFileAll(filepath, buffers[bufferId].second)) {
            std::cout << "[Export] Saved to: " << filepath << "\n";
        }
        else {
            std::cout << "[Export] Failed to write: " << filepath << "\n";
        }
    }

    // Editing helpers
    static std::vector<std::string> SplitLines(const std::string& s) {
        std::vector<std::string> out;
        std::istringstream iss(s);
        std::string line;
        while (std::getline(iss, line)) out.push_back(line);
        if (!s.empty() && s.back() == '\n') out.push_back(std::string{});
        return out;
    }

    static std::string JoinLines(const std::vector<std::string>& lines) {
        std::ostringstream oss;
        for (size_t i = 0; i < lines.size(); ++i) {
            oss << lines[i];
            if (i + 1 < lines.size()) oss << '\n';
        }
        return oss.str();
    }

    static void SetActiveBufferText(const std::string& text, bool markModified) {
        if (buffers.empty()) return;
        buffers[activeBuffer].second = text;
        if (markModified) isModified = true;
        RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
    }

    // Prompt
    static std::string PromptLine(const char* prompt) {
        std::cout << prompt;
        std::cout.flush();
        if (!std::cin.good()) std::cin.clear();
        std::string s;
        std::getline(std::cin, s);
        return s;
    }

    // [U] Custom paging prompt
    static void PromptCustomPaging() {
        auto askULong = [&](const std::string& prompt, std::optional<size_t> dflt) -> std::optional<size_t> {
            std::string p = prompt;
            if (dflt) p += " [" + std::to_string(*dflt) + "]";
            p += ": ";
            std::string in = PromptLine(p.c_str());
            if (in.empty() && dflt) return dflt;
            if (in.empty()) return std::nullopt;
            try { return static_cast<size_t>(std::stoull(in)); }
            catch (...) { return std::nullopt; }
            };

        auto maxLines = askULong("Max lines to render", std::nullopt);
        if (!maxLines || *maxLines == 0) {
            std::cout << "[Custom Paging] Canceled.\n";
            return;
        }
        auto startLine = askULong("Start at line (0-based)", 0);
        if (!startLine) startLine = 0;
        auto spacing = askULong("Render spacing (1=dense, >1 adds gaps)", gCfg.defaultSpacing);
        if (!spacing) spacing = gCfg.defaultSpacing;

        std::cout << "[Custom Paging] Rendering lines " << *startLine << "…"
            << (*startLine + *maxLines - 1) << " (spacing " << *spacing << ").\n";
        RenderActiveBufferPagedImpl(*startLine, *maxLines, *spacing);
    }

    // ============================================================
    // Interactive "Z" editor helpers (raw mode, keys, viewport)
    // ============================================================

    enum class Key {
        Unknown,
        Up, Down, Left, Right,
        PageUp, PageDown, Home, End,
        Enter, Escape,
        Char,
        CtrlS,
        CtrlQ
    };

    struct TermSizeSimple {
        int cols = 80;
        int rows = 24;
    };

    static TermSizeSimple GetTerminalSizeNow() {
        TermSizeSimple ts;
#if defined(_WIN32)
        CONSOLE_SCREEN_BUFFER_INFO info;
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (GetConsoleScreenBufferInfo(hOut, &info)) {
            ts.cols = info.srWindow.Right - info.srWindow.Left + 1;
            ts.rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        }
#else
        struct winsize ws {};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
            if (ws.ws_col) ts.cols = ws.ws_col;
            if (ws.ws_row) ts.rows = ws.ws_row;
        }
#endif
        return ts;
    }

#if defined(_WIN32)
    struct RawModeGuard {
        DWORD inOld = 0, outOld = 0;
        bool active = false;
        RawModeGuard() {
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            if (!GetConsoleMode(hIn, &inOld)) return;
            if (!GetConsoleMode(hOut, &outOld)) return;

            DWORD inNew = inOld;
            inNew |= ENABLE_VIRTUAL_TERMINAL_INPUT;
            inNew &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
            SetConsoleMode(hIn, inNew);

            DWORD outNew = outOld | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, outNew);

            active = true;
        }
        ~RawModeGuard() {
            if (!active) return;
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
            SetConsoleMode(hIn, inOld);
            SetConsoleMode(hOut, outOld);
        }
    };

    static int ReadByteWin() {
        INPUT_RECORD rec;
        DWORD read = 0;
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        for (;;) {
            if (!ReadConsoleInput(hIn, &rec, 1, &read)) return -1;
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
                WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
                CHAR ch = rec.Event.KeyEvent.uChar.AsciiChar;
                switch (vk) {
                case VK_UP:    return 0x1001;
                case VK_DOWN:  return 0x1002;
                case VK_LEFT:  return 0x1003;
                case VK_RIGHT: return 0x1004;
                case VK_PRIOR: return 0x1005; // PageUp
                case VK_NEXT:  return 0x1006; // PageDown
                case VK_HOME:  return 0x1007;
                case VK_END:   return 0x1008;
                case VK_RETURN:return '\r';
                case VK_ESCAPE:return 0x1B;
                default:
                    if (vk == 'S' && (rec.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
                        return 0x1011; // CtrlS
                    if (vk == 'Q' && (rec.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)))
                        return 0x1012; // CtrlQ
                    if (ch) return (unsigned char)ch;
                }
            }
        }
    }
#else
    struct RawModeGuard {
        termios old{};
        bool active = false;
        RawModeGuard() {
            if (!isatty(STDIN_FILENO)) return;
            if (tcgetattr(STDIN_FILENO, &old) != 0) return;
            termios raw = old;
            raw.c_lflag &= ~(ECHO | ICANON);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return;
            active = true;
        }
        ~RawModeGuard() {
            if (active) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        }
    };
#endif

    static int ReadByteAny() {
#if defined(_WIN32)
        return ReadByteWin();
#else
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) return -1;
        return (int)c;
#endif
    }

    static Key ReadKey(char& outChar) {
        outChar = 0;
#if defined(_WIN32)
        int b = ReadByteAny();
        if (b == -1) return Key::Unknown;
        switch (b) {
        case 0x1001: return Key::Up;
        case 0x1002: return Key::Down;
        case 0x1003: return Key::Left;
        case 0x1004: return Key::Right;
        case 0x1005: return Key::PageUp;
        case 0x1006: return Key::PageDown;
        case 0x1007: return Key::Home;
        case 0x1008: return Key::End;
        case 0x1011: return Key::CtrlS;
        case 0x1012: return Key::CtrlQ;
        case '\r':   return Key::Enter;
        case 0x1B:   return Key::Escape;
        default:
            if (b >= 32 && b < 127) { outChar = (char)b; return Key::Char; }
            return Key::Unknown;
        }
#else
        int c = ReadByteAny();
        if (c == -1) return Key::Unknown;
        if (c == 0x1B) { // ESC sequence
            int c1 = ReadByteAny();
            if (c1 == '[') {
                int c2 = ReadByteAny();
                if (c2 == 'A') return Key::Up;
                if (c2 == 'B') return Key::Down;
                if (c2 == 'C') return Key::Right;
                if (c2 == 'D') return Key::Left;
                if (c2 == 'H') return Key::Home;
                if (c2 == 'F') return Key::End;
                if (c2 == '5') { ReadByteAny(); return Key::PageUp; }   // consumes '~'
                if (c2 == '6') { ReadByteAny(); return Key::PageDown; } // consumes '~'
            }
            return Key::Escape;
        }
        if (c == '\r' || c == '\n') return Key::Enter;
        if (c == 19) return Key::CtrlS; // ^S
        if (c == 17) return Key::CtrlQ; // ^Q
        if (c >= 32 && c < 127) { outChar = (char)c; return Key::Char; }
        return Key::Unknown;
#endif
    }

    static void AnsiClearScreen() {
        std::cout << "\x1b[2J\x1b[H";
    }
    static void AnsiHideCursor(bool hide) {
        std::cout << (hide ? "\x1b[?25l" : "\x1b[?25h");
    }
    static void AnsiSetInverse(bool on) {
        std::cout << (on ? "\x1b[7m" : "\x1b[27m");
    }

    static void DrawViewport(const std::vector<std::string>& lines,
        size_t top, size_t cursor,
        const std::string& headerLeft,
        const std::string& headerRight,
        bool dirty)
    {
        TermSizeSimple ts = GetTerminalSizeNow();
        int rows = (std::max)(8, ts.rows);
        int cols = (std::max)(40, ts.cols);

        int headerRows = 2;     // title + hint
        int statusRows = 1;     // status bar
        int viewRows = rows - headerRows - statusRows;
        if (viewRows < 3) viewRows = 3;

        AnsiClearScreen();

        // Header
        std::string title = headerLeft;
        if ((int)title.size() > cols) title.resize(cols);
        std::cout << title;
        if (!headerRight.empty()) {
            int pad = cols - (int)title.size() - (int)headerRight.size();
            if (pad > 0) std::cout << std::string(pad, ' ');
            std::cout << headerRight;
        }
        std::cout << "\n";
        // Swapped ↑ and ↓ for ASCII-safe caret/up and lowercase v
        std::cout << "^/v move  Enter edit  I insert  o append  d delete  : cmd  b bulk  ? help\n";

        // Viewport
        size_t total = lines.size();
        size_t end = (std::min)(top + (size_t)viewRows, total);
        for (size_t i = top; i < end; ++i) {
            bool cur = (i == cursor);
            if (cur) AnsiSetInverse(true);
            std::string prefix = std::to_string(i + 1) + ": ";
            std::string content = (i < lines.size() ? lines[i] : "");
            if ((int)(prefix.size() + content.size()) > cols) {
                int avail = cols - (int)prefix.size();
                if (avail < 0) avail = 0;
                if (avail < (int)content.size()) content.resize(avail);
            }
            std::cout << prefix << content << (cur ? "\x1b[0m" : "") << "\n";
            if (cur) AnsiSetInverse(false);
        }
        for (int r = (int)(end - top); r < viewRows; ++r) std::cout << "~\n";

        // Status bar
        std::string dirtyMark = dirty ? "*" : "-";
        std::string status = "[Z-Edit " + dirtyMark + "] " +
            std::to_string(cursor + 1) + "/" + std::to_string(total);
        if ((int)status.size() > cols) status.resize(cols);
        AnsiSetInverse(true);
        std::cout << status;
        if ((int)status.size() < cols) std::cout << std::string(cols - (int)status.size(), ' ');
        std::cout << "\x1b[0m";
        std::cout.flush();
    }

    // ============================================================

    void LaunchInteractiveUI() {
        // Initialize adaptive state
        gDfl = AdaptiveDefaults{};
        gCfg = MakeAdaptiveConfig(gDfl);
        gTerm = DetectTerminal();
        gSys = DetectSystem();
        gDec = DecidePageCap(gDfl, gCfg, gTerm, gSys);

        // Diagnostics banner + key help
        PrintDiagnosticsBanner();
        std::cout << "[S: Search] [H: Highlight] [C: Compile] [E: Export] [R: Repaint] [V: View All] [U: Custom Page] [O: Open] [T: Switch] [Q: Quit]\n"
            << "[W: Save] [A: Save As] [I: Insert line] [D: Delete line] [M: Modify line] [Z: Free Edit] [P: Prefs] [B: Binary]\n";

        // Initial render (uses gDec)
        RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);

        bool running = true;
        while (running) {
            std::cout << "\n[Await command] > ";
            std::cout.flush();

            char ch = 0;
            if (!(std::cin >> ch)) {
                std::cin.clear();
                break;
            }
            std::cin.ignore((std::numeric_limits<std::streamsize>::max)(), '\n');

            switch (std::toupper(static_cast<unsigned char>(ch))) {
            case 'Q':
                running = false;
                break;
            case 'S':
                SearchInBuffer(PromptLine("Search: "));
                break;
            case 'H':
                Highlight(PromptLine("Highlight: "));
                break;
            case 'C':
                CompileCurrentBuffer();
                break;
            case 'E':
                ExportBufferToFile(activeBuffer, PromptLine("Export path: "));
                break;
            case 'R': {
                // Re-detect terminal (e.g., after resize/SSH) and repaint
                gTerm = DetectTerminal();
                gSys = DetectSystem();
                gCfg = MakeAdaptiveConfig(gDfl); // allow env/props changes mid-session
                gDec = DecidePageCap(gDfl, gCfg, gTerm, gSys);
                PrintDiagnosticsBanner();
                RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
                break;
            }
            case 'B': {
                if (buffers.empty()) { std::cout << "[Binary] No active buffer.\n"; break; }
                std::string path = buffers[activeBuffer].first;
                std::cout << "[Binary] Target: " << path << "\n";
                std::cout << "1) Probe  2) Translate(static)  3) Rewrite(static)  4) Interpret  5) Emulate  6) VirtAssist\n";
                std::string sel = PromptLine("Select: ");
                if (sel == "1") {
                    if (auto bi = BinaryManip::Probe(path)) {
                        std::cout << "Arch=" << (int)bi->arch << " OS=" << (int)bi->os << " lib=" << (bi->isLibrary ? "yes" : "no") << "\n";
                        auto secs = BinaryManip::ListSections(path);
                        std::cout << "Sections: "; for (auto& s : secs) std::cout << s << " "; std::cout << "\n";
                    }
                    else std::cout << "[Binary] Probe failed.\n";
                }
                else if (sel == "2") {
                    BinaryManip::TranslateOpts to; to.mode = BinaryManip::Mode::Static; to.targetArch = BinaryManip::Arch::X64;
                    auto r = BinaryManip::Translate(path, to);
                    std::cout << (r.ok ? "[OK] " : "[ERR] ") << r.message << (r.outputPath ? (" | " + *r.outputPath) : "") << "\n";
                }
                else if (sel == "3") {
                    BinaryManip::RewriteOpts ro; ro.mode = BinaryManip::Mode::Static; ro.inst.profile = true;
                    auto r = BinaryManip::Rewrite(path, ro);
                    std::cout << (r.ok ? "[OK] " : "[ERR] ") << r.message << (r.outputPath ? (" | " + *r.outputPath) : "") << "\n";
                }
                else if (sel == "4") {
                    BinaryManip::InterpretOpts io; io.collectTrace = true; io.liftHotPaths = true;
                    auto r = BinaryManip::Interpret(path, io);
                    std::cout << (r.ok ? "[OK] " : "[ERR] ") << r.message << "\n";
                }
                else if (sel == "5") {
                    BinaryManip::EmulateOpts eo; eo.fullSystem = false;
                    auto r = BinaryManip::Emulate(path, eo);
                    std::cout << (r.ok ? "[OK] " : "[ERR] ") << r.message << "\n";
                }
                else if (sel == "6") {
                    BinaryManip::VirtAssistOpts vo; vo.rewriteSensitive = true;
                    auto r = BinaryManip::VirtAssist(path, vo);
                    std::cout << (r.ok ? "[OK] " : "[ERR] ") << r.message << "\n";
                }
                else {
                    std::cout << "[Binary] Unknown selection.\n";
                }
                break;
            }

            case 'U':
                PromptCustomPaging();
                break;
            case 'V': {
                if (buffers.empty()) break;
                size_t total = CountLines(buffers[activeBuffer].second);
                const size_t safeThreshold = 10000; // guard against flooding

                if (total > safeThreshold) {
                    std::cout << "[Warning] Buffer has " << total << " lines. Rendering all may be slow and flood the console.\n";
                    std::string resp = PromptLine("Continue anyway? (y/N): ");
                    if (resp.empty() || std::toupper(static_cast<unsigned char>(resp[0])) != 'Y') {
                        std::cout << "[View All] Cancelled.\n";
                        break;
                    }
                }
                size_t viewAll = (std::min)(gCfg.hardCap ? gCfg.hardCap : gDfl.hardCap, total);
                if (viewAll < total) {
                    std::cout << "[View All] Clamped to hard_cap=" << (gCfg.hardCap ? gCfg.hardCap : gDfl.hardCap) << ".\n";
                }
                RenderActiveBufferPagedImpl(0, viewAll, gCfg.defaultSpacing);
                break;
            }
            case 'O': {
                std::string path = PromptLine("Open file: ");
                if (path.empty()) break;
                std::string raw;
                if (ReadFileAll(path, raw)) {
                    auto style = DetectNewlineStyle(raw);
                    std::string content = NormalizeNewlines(raw);
                    OpenBuffer(path, content);
                    RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
                    size_t lines = CountLines(content);
                    std::cout << "\n[Open] " << path
                        << " | bytes: " << raw.size()
                        << " | newline: " << StyleName(style)
                        << " | lines: " << lines << "\n";
                }
                else {
                    std::cout << "[Open] Cannot open: " << path << "\n";
                }
                break;
            }
            case 'T':
                if (!buffers.empty()) {
                    size_t next = (activeBuffer + 1) % buffers.size();
                    SwitchToBuffer(next);
                }
                break;
            case 'W':
                if (!buffers.empty()) {
                    if (WriteFileAll(buffers[activeBuffer].first, buffers[activeBuffer].second)) {
                        std::cout << "[Save] Wrote " << buffers[activeBuffer].first << "\n";
                        isModified = false;
                        RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
                    }
                    else {
                        std::cout << "[Save] Failed to write " << buffers[activeBuffer].first << "\n";
                    }
                }
                break;
            case 'A': {
                if (buffers.empty()) break;
                std::string path = PromptLine("Save As path: ");
                if (!path.empty() && WriteFileAll(path, buffers[activeBuffer].second)) {
                    std::cout << "[Save As] Wrote " << path << "\n";
                }
                else {
                    std::cout << "[Save As] Failed to write " << path << "\n";
                }
                break;
            }
            case 'P': {
                std::string prompt = "Auto-save after Z? (Y/N) [current: ";
                prompt += (autoSaveAfterZ ? "Y" : "N");
                prompt += "]: ";
                std::string ans = PromptLine(prompt.c_str());
                if (!ans.empty()) {
                    char c = std::toupper(static_cast<unsigned char>(ans[0]));
                    if (c == 'Y') autoSaveAfterZ = true;
                    else if (c == 'N') autoSaveAfterZ = false;
                }
                std::cout << "[Prefs] Auto-save after Z: " << (autoSaveAfterZ ? "On" : "Off") << "\n";
                break;
            }
            case 'Z': {
                if (buffers.empty()) break;
                std::cout << "Z-mode options:\n"
                    "  1) Line/page navigation (legacy line-oriented)\n"
                    "  2) Full in-place editing window (free 2D navigation + typing)\n";
                std::string modeChoice = PromptLine("Select [1-2] (Enter=cancel): ");
                if (modeChoice.empty()) { std::cout << "[Z] Cancelled.\n"; break; }
                std::vector<std::string> work = SplitLines(buffers[activeBuffer].second);
                bool dirty = false;
                size_t total = work.size();
                const size_t safeThreshold = 10000;
                if (total > safeThreshold) {
                    std::string resp = PromptLine("[Warning] Very large buffer. Continue into Z? (y/N): ");
                    if (resp.empty() || std::toupper((unsigned char)resp[0]) != 'Y') {
                        std::cout << "[Z] Cancelled.\n";
                        break;
                    }
                }
                auto commitToBuffer = [&]() {
                    std::string newText = JoinLines(work);
                    SetActiveBufferText(newText, true);
                    };

                if (modeChoice == "1") {
                    size_t cursor = 0;
                    size_t top = 0;
                    auto clampScroll = [&](int viewRows) {
                        if (work.empty()) { cursor = 0; top = 0; return; }
                        if (cursor >= work.size()) cursor = work.size() - 1;
                        size_t viewRowsSz = (size_t)(std::max)(3, viewRows);
                        if (cursor < top) top = cursor;
                        if (cursor >= top + viewRowsSz) top = cursor - (viewRowsSz - 1);
                        };
                    auto redraw = [&]() {
                        TermSizeSimple ts = GetTerminalSizeNow();
                        int rows = (std::max)(8, ts.rows);
                        int viewRows = (std::max)(3, rows - 2 - 1);
                        clampScroll(viewRows);
                        std::string headerLeft = "[Z] " + buffers[activeBuffer].first;
                        std::string headerRight = "AutoSave: " + std::string(autoSaveAfterZ ? "On" : "Off");
                        DrawViewport(work, top, cursor, headerLeft, headerRight, dirty);
                        };
                    {
                        RawModeGuard raw;
                        AnsiHideCursor(true);
                        auto readCooked = [&](const std::string& prompt) -> std::string {
                            AnsiHideCursor(false);
                            raw.~RawModeGuard();
                            std::cout << "\n" << prompt;
                            std::cout.flush();
                            std::string s;
                            std::getline(std::cin, s);
                            new (&raw) RawModeGuard();
                            AnsiHideCursor(true);
                            return s;
                            };
                        auto confirmDiscard = [&]() -> bool {
                            AnsiHideCursor(false);
                            raw.~RawModeGuard();
                            std::string choice = PromptLine("Discard changes? (y/N): ");
                            new (&raw) RawModeGuard();
                            AnsiHideCursor(true);
                            return !choice.empty() && std::toupper((unsigned char)choice[0]) == 'Y';
                            };
                        auto bulkPaste = [&]() {
                            std::cout << "\nEnter full content. End with a single '.' on its own line.\n";
                            raw.~RawModeGuard();
                            std::vector<std::string> pasted;
                            std::string line;
                            while (std::getline(std::cin, line)) {
                                if (line == ".") break;
                                pasted.push_back(line);
                            }
                            new (&raw) RawModeGuard();
                            work.swap(pasted);
                            if (cursor >= work.size()) cursor = work.empty() ? 0 : work.size() - 1;
                            top = 0;
                            dirty = true;
                            };
                        redraw();
                        while (true) {
                            char ch2 = 0;
                            Key k = ReadKey(ch2);
                            if (k == Key::Up) { if (cursor > 0) cursor--; redraw(); }
                            else if (k == Key::Down) { if (!work.empty() && cursor + 1 < work.size()) cursor++; redraw(); }
                            else if (k == Key::PageUp) {
                                TermSizeSimple ts = GetTerminalSizeNow();
                                int viewRows = (std::max)(3, ts.rows - 2 - 1);
                                size_t jump = (size_t)(std::max)(3, viewRows);
                                cursor = (cursor >= jump) ? cursor - jump : 0;
                                redraw();
                            }
                            else if (k == Key::PageDown) {
                                TermSizeSimple ts = GetTerminalSizeNow();
                                int viewRows = (std::max)(3, ts.rows - 2 - 1);
                                size_t jump = (size_t)(std::max)(3, viewRows);
                                if (!work.empty())
                                    cursor = (std::min)(work.size() - 1, cursor + jump);
                                redraw();
                            }
                            else if (k == Key::Home) { if (cursor != 0) { cursor = 0; redraw(); } }
                            else if (k == Key::End) { if (!work.empty() && cursor != work.size() - 1) { cursor = work.size() - 1; redraw(); } }
                            else if (k == Key::Enter) {
                                if (work.empty()) {
                                    std::string s = readCooked("Insert line 1: ");
                                    if (!s.empty()) { work.push_back(s); dirty = true; }
                                }
                                else {
                                    std::string s = readCooked("Edit line " + std::to_string(cursor + 1) + " (blank = keep): ");
                                    if (!s.empty()) { work[cursor] = s; dirty = true; }
                                }
                                redraw();
                            }
                            else if (k == Key::Char) {
                                if (ch2 == 'd') {
                                    if (!work.empty()) {
                                        work.erase(work.begin() + (long long)cursor);
                                        if (cursor >= work.size() && !work.empty()) cursor = work.size() - 1;
                                        dirty = true;
                                        redraw();
                                    }
                                }
                                else if (ch2 == 'i') {
                                    std::string s = readCooked("Insert before line " + std::to_string(cursor + 1) + ": ");
                                    work.insert(work.begin() + (long long)cursor, s);
                                    dirty = true;
                                    redraw();
                                }
                                else if (ch2 == 'o') {
                                    std::string s = readCooked("Append after line " + std::to_string(cursor + 1) + ": ");
                                    size_t pos = work.empty() ? 0 : (cursor + 1);
                                    work.insert(work.begin() + (long long)pos, s);
                                    cursor = pos;
                                    dirty = true;
                                    redraw();
                                }
                                else if (ch2 == 'b') { bulkPaste(); redraw(); }
                                else if (ch2 == '?') {
                                    AnsiClearScreen();
                                    std::cout <<
                                        "Z Interactive Help (Legacy)\n"
                                        "---------------------------\n"
                                        "Arrows/Home/End/PgUp/PgDn: move\n"
                                        "Enter: edit current line (blank keeps)\n"
                                        "i: insert before | o: append after | d: delete current\n"
                                        "b: bulk replace (end with '.')\n"
                                        ": enter command (:w, :x, :wq, :q!, :gN)\n"
                                        "q or Esc: exit (prompts if unsaved)\n\n"
                                        "Press any key to return...\n";
                                    std::cout.flush();
                                    char tmp = 0; ReadKey(tmp);
                                    redraw();
                                }
                                else if (ch2 == ':') {
                                    std::string cmd = readCooked(":");
                                    std::string cmdLower = cmd;
                                    for (char& c2 : cmdLower) c2 = (char)std::tolower((unsigned char)c2);

                                    if (cmdLower == "q!") { dirty = false; break; }
                                    else if (cmdLower == "w" || cmdLower == "x" || cmdLower == "wq") {
                                        commitToBuffer(); dirty = false;
                                        bool exiting = (cmdLower == "x" || cmdLower == "wq");
                                        if (autoSaveAfterZ) {
                                            if (SaveActiveBufferToDisk()) { isModified = false; if (exiting) break; redraw(); }
                                            else { std::cout << "[Z] Failed to save to disk.\n"; if (exiting) break; redraw(); }
                                        }
                                        else if (exiting) {
                                            std::string choice = readCooked("Save changes now? (y/N): ");
                                            if (!choice.empty() && std::toupper((unsigned char)choice[0]) == 'Y') {
                                                if (SaveActiveBufferToDisk()) isModified = false;
                                                else std::cout << "[Z] Failed to save to disk.\n";
                                            }
                                            break;
                                        }
                                        else {
                                            std::cout << "[Z] Buffer updated (not exiting). Auto-save is Off.\n";
                                            redraw();
                                        }
                                    }
                                    else if (!cmd.empty() && (cmd[0] == 'g' || cmd[0] == 'G')) {
                                        try {
                                            size_t n = std::stoul(cmd.substr(1));
                                            if (n >= 1 && n <= work.size()) cursor = n - 1;
                                        }
                                        catch (...) {}
                                        redraw();
                                    }
                                    else { redraw(); }
                                }
                                else if (ch2 == 'q') {
                                    if (dirty) { if (confirmDiscard()) { dirty = false; break; } else redraw(); }
                                    else break;
                                }
                            }
                            else if (k == Key::CtrlS) { commitToBuffer(); dirty = false; redraw(); }
                            else if (k == Key::Escape || k == Key::CtrlQ) {
                                if (dirty) { if (confirmDiscard()) { dirty = false; break; } else redraw(); }
                                else break;
                            }
                        }
                    }
                    RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
                    break;
                }

                if (modeChoice != "2") {
                    std::cout << "[Z] Invalid choice.\n";
                    break;
                }

                size_t cursorLine = 0;
                size_t cursorCol = 0;
                size_t top = 0;
                size_t horizScroll = 0;
                bool insertMode = true;

                auto clampLineCol = [&]() {
                    if (work.empty()) { cursorLine = 0; cursorCol = 0; return; }
                    if (cursorLine >= work.size()) cursorLine = work.size() - 1;
                    const std::string& s = work[cursorLine];
                    if (cursorCol > s.size()) cursorCol = s.size();
                    };

                auto computeView = [&]() -> std::tuple<int, int, int> {
                    TermSizeSimple ts = GetTerminalSizeNow();
                    int rows = (std::max)(8, ts.rows);
                    int cols = (std::max)(40, ts.cols);
                    int headerRows = 2;
                    int helpRows = 1;
                    int statusRows = 1;
                    int viewRows = rows - headerRows - helpRows - statusRows;
                    if (viewRows < 3) viewRows = 3;
                    return { rows, cols, viewRows };
                    };

                auto clampScroll = [&](int viewRows) {
                    if (work.empty()) { cursorLine = 0; top = 0; return; }
                    size_t viewRowsSz = (size_t)(std::max)(3, viewRows);
                    if (cursorLine < top) top = cursorLine;
                    if (cursorLine >= top + viewRowsSz) top = cursorLine - (viewRowsSz - 1);
                    };

                auto ensureCaretVisibleHoriz = [&](int cols) {
                    std::string prefix = std::to_string(cursorLine + 1) + ": ";
                    int leftPad = (int)prefix.size();
                    int contentWidth = cols - leftPad;
                    if (contentWidth < 8) contentWidth = 8;
                    if (cursorCol < horizScroll) horizScroll = cursorCol;
                    else if ((cursorCol - horizScroll) >= (size_t)contentWidth) {
                        horizScroll = cursorCol - (size_t)contentWidth + 1;
                    }
                    };

                auto drawWithCaret = [&]() {
                    auto [rows, cols, viewRows] = computeView();
                    clampLineCol();
                    clampScroll(viewRows);
                    ensureCaretVisibleHoriz(cols);

                    AnsiClearScreen();
                    std::string headerLeft = "[Z] " + buffers[activeBuffer].first;
                    std::string headerRight = "AutoSave: " + std::string(autoSaveAfterZ ? "On" : "Off");
                    std::string title = headerLeft;
                    if ((int)title.size() > cols) title.resize(cols);
                    std::cout << title;
                    if (!headerRight.empty()) {
                        int pad = cols - (int)title.size() - (int)headerRight.size();
                        if (pad > 0) std::cout << std::string(pad, ' ');
                        std::cout << headerRight;
                    }
                    std::cout << "\n";

                    std::string help = "^/v, PgUp/PgDn: move  Left/Right: col  Home/End: first/last line  Ins: INS/OVR  i/o/d/b  : cmd  q/Esc  Ctrl+S";
                    if ((int)help.size() > cols) help.resize(cols);
                    std::cout << help << "\n";

                    size_t totalLines = work.size();
                    size_t end = (std::min)(top + (size_t)viewRows, totalLines);
                    for (size_t i = top; i < end; ++i) {
                        std::string prefix = std::to_string(i + 1) + ": ";
                        std::string line = (i < work.size() ? work[i] : "");
                        std::string slice;
                        if (horizScroll < line.size()) slice = line.substr(horizScroll);
                        int contentAvail = cols - (int)prefix.size();
                        if (contentAvail < 0) contentAvail = 0;
                        if ((int)slice.size() > contentAvail) slice.resize(contentAvail);

                        if (i == cursorLine) {
                            size_t caretInSlice = (cursorCol >= horizScroll) ? (cursorCol - horizScroll) : (size_t)SIZE_MAX;
                            std::string leftPart = (caretInSlice != (size_t)SIZE_MAX && caretInSlice <= slice.size())
                                ? slice.substr(0, (std::min)(caretInSlice, slice.size()))
                                : slice;
                            std::string caretChar;
                            if (caretInSlice != (size_t)SIZE_MAX && caretInSlice < slice.size()) caretChar = slice.substr(caretInSlice, 1);
                            else if (caretInSlice == slice.size()) caretChar = " ";
                            std::string rightPart;
                            if (caretInSlice != (size_t)SIZE_MAX && caretInSlice < slice.size()) rightPart = slice.substr(caretInSlice + 1);

                            std::cout << prefix << leftPart;
                            AnsiSetInverse(true); std::cout << caretChar; AnsiSetInverse(false);
                            std::cout << rightPart << "\n";
                        }
                        else {
                            std::cout << prefix << slice << "\n";
                        }
                    }
                    for (int r = (int)(end - top); r < viewRows; ++r) std::cout << "~\n";

                    std::string dirtyMark = dirty ? "*" : "-";
                    std::string modeMark = insertMode ? "INS" : "OVR";
                    std::string status = "[Z-Edit " + dirtyMark + "] " +
                        std::to_string(cursorLine + 1) + "/" + std::to_string(totalLines) +
                        "  col " + std::to_string(cursorCol + 1) +
                        "  hscroll " + std::to_string(horizScroll) +
                        "  " + modeMark;
                    if ((int)status.size() > cols) status.resize(cols);
                    AnsiSetInverse(true);
                    std::cout << status;
                    if ((int)status.size() < cols) std::cout << std::string(cols - (int)status.size(), ' ');
                    std::cout << "\x1b[0m";
                    std::cout.flush();
                    };

                {
                    RawModeGuard raw;
                    AnsiHideCursor(true);

                    auto readCooked = [&](const std::string& prompt) -> std::string {
                        AnsiHideCursor(false);
                        raw.~RawModeGuard();
                        std::cout << "\n" << prompt;
                        std::cout.flush();
                        std::string s;
                        std::getline(std::cin, s);
                        new (&raw) RawModeGuard();
                        AnsiHideCursor(true);
                        return s;
                        };

                    auto confirmDiscard = [&]() -> bool {
                        AnsiHideCursor(false);
                        raw.~RawModeGuard();
                        std::string choice = PromptLine("Discard changes? (y/N): ");
                        new (&raw) RawModeGuard();
                        AnsiHideCursor(true);
                        return !choice.empty() && std::toupper((unsigned char)choice[0]) == 'Y';
                        };

                    auto bulkPaste = [&]() {
                        std::cout << "\nEnter full content. End with a single '.' on its own line.\n";
                        raw.~RawModeGuard();
                        std::vector<std::string> pasted;
                        std::string line;
                        while (std::getline(std::cin, line)) {
                            if (line == ".") break;
                            pasted.push_back(line);
                        }
                        new (&raw) RawModeGuard();
                        work.swap(pasted);
                        if (cursorLine >= work.size()) cursorLine = work.empty() ? 0 : work.size() - 1;
                        cursorCol = 0;
                        top = 0;
                        horizScroll = 0;
                        dirty = true;
                        };

                    drawWithCaret();

                    while (true) {
                        char ch2 = 0;
                        Key k = ReadKey(ch2);

                        if (k == Key::Up) {
                            if (!work.empty() && cursorLine > 0) {
                                cursorLine--;
                                if (cursorCol > work[cursorLine].size()) cursorCol = work[cursorLine].size();
                            }
                            drawWithCaret();
                        }
                        else if (k == Key::Down) {
                            if (!work.empty() && cursorLine + 1 < work.size()) {
                                cursorLine++;
                                if (cursorCol > work[cursorLine].size()) cursorCol = work[cursorLine].size();
                            }
                            drawWithCaret();
                        }
                        else if (k == Key::PageUp) {
                            auto [rows, cols, viewRows] = computeView();
                            size_t jump = (size_t)(std::max)(3, viewRows);
                            cursorLine = (cursorLine >= jump) ? cursorLine - jump : 0;
                            if (!work.empty() && cursorCol > work[cursorLine].size()) cursorCol = work[cursorLine].size();
                            drawWithCaret();
                        }
                        else if (k == Key::PageDown) {
                            auto [rows, cols, viewRows] = computeView();
                            size_t jump = (size_t)(std::max)(3, viewRows);
                            if (!work.empty())
                                cursorLine = (std::min)(work.size() - 1, cursorLine + jump);
                            if (!work.empty() && cursorCol > work[cursorLine].size()) cursorCol = work[cursorLine].size();
                            drawWithCaret();
                        }
                        else if (k == Key::Home) {
                            if (!work.empty() && cursorLine != 0) {
                                cursorLine = 0;
                                if (cursorCol > work[cursorLine].size()) cursorCol = work[cursorLine].size();
                                drawWithCaret();
                            }
                        }
                        else if (k == Key::End) {
                            if (!work.empty() && cursorLine != work.size() - 1) {
                                cursorLine = work.size() - 1;
                                if (cursorCol > work[cursorLine].size()) cursorCol = work[cursorLine].size();
                                drawWithCaret();
                            }
                        }
                        else if (k == Key::Left) {
                            if (cursorCol > 0) cursorCol--;
                            drawWithCaret();
                        }
                        else if (k == Key::Right) {
                            if (!work.empty()) {
                                const std::string& s = work[cursorLine];
                                if (cursorCol < s.size()) cursorCol++;
                            }
                            drawWithCaret();
                        }
                        else if (k == Key::CtrlS) {
                            commitToBuffer();
                            dirty = false;
                            drawWithCaret();
                        }
                        else if (k == Key::CtrlQ || k == Key::Escape) {
                            if (dirty) { if (confirmDiscard()) { dirty = false; break; } else drawWithCaret(); }
                            else break;
                        }
                        else if (k == Key::Char) {
                            if (ch2 == 'i') {
                                std::string s = readCooked("Insert before line " + std::to_string(cursorLine + 1) + ": ");
                                work.insert(work.begin() + (long long)cursorLine, s);
                                cursorCol = 0;
                                dirty = true;
                                drawWithCaret();
                            }
                            else if (ch2 == 'o') {
                                std::string s = readCooked("Append after line " + std::to_string(cursorLine + 1) + ": ");
                                size_t pos = work.empty() ? 0 : (cursorLine + 1);
                                work.insert(work.begin() + (long long)pos, s);
                                cursorLine = pos;
                                cursorCol = 0;
                                dirty = true;
                                drawWithCaret();
                            }
                            else if (ch2 == 'd') {
                                if (!work.empty()) {
                                    work.erase(work.begin() + (long long)cursorLine);
                                    if (cursorLine >= work.size()) cursorLine = work.empty() ? 0 : work.size() - 1;
                                    cursorCol = 0;
                                    dirty = true;
                                    drawWithCaret();
                                }
                            }
                            else if (ch2 == 'b') {
                                bulkPaste();
                                drawWithCaret();
                            }
                            else if (ch2 == '?') {
                                AnsiClearScreen();
                                std::cout <<
                                    "Z Interactive Help (Edit Window)\n"
                                    "--------------------------------\n"
                                    "Movement: Up/Down, Left/Right, Home/End, PgUp/PgDn\n"
                                    "Typing: insert/overwrite at caret (toggle with Ins)\n"
                                    "Backspace/Delete: remove left/at caret\n"
                                    "Enter: quick replace whole line via prompt (optional)\n"
                                    "i/o/d: insert/append/delete line\n"
                                    "b: bulk replace (end with '.')\n"
                                    ": commands (:w, :x, :wq, :q!, :gN)\n"
                                    "Ctrl+S: save to buffer (in-memory)\n"
                                    "q or Esc: exit (prompts if unsaved)\n\n"
                                    "Press any key to return...\n";
                                std::cout.flush();
                                char tmp = 0; ReadKey(tmp);
                                drawWithCaret();
                            }
                            else if (ch2 == ':') {
                                std::string cmd = readCooked(":");
                                std::string cmdLower = cmd;
                                for (char& c2 : cmdLower) c2 = (char)std::tolower((unsigned char)c2);

                                if (cmdLower == "q!") { dirty = false; break; }
                                else if (cmdLower == "w" || cmdLower == "x" || cmdLower == "wq") {
                                    commitToBuffer();
                                    dirty = false;

                                    bool exiting = (cmdLower == "x" || cmdLower == "wq");
                                    if (autoSaveAfterZ) {
                                        if (SaveActiveBufferToDisk()) { isModified = false; if (exiting) break; drawWithCaret(); }
                                        else { std::cout << "[Z] Failed to save to disk.\n"; if (exiting) break; drawWithCaret(); }
                                    }
                                    else if (exiting) {
                                        std::string choice = readCooked("Save changes now? (y/N): ");
                                        if (!choice.empty() && std::toupper((unsigned char)choice[0]) == 'Y') {
                                            if (SaveActiveBufferToDisk()) isModified = false;
                                            else std::cout << "[Z] Failed to save to disk.\n";
                                        }
                                        break;
                                    }
                                    else {
                                        std::cout << "[Z] Buffer updated (not exiting). Auto-save is Off.\n";
                                        drawWithCaret();
                                    }
                                }
                                else if (!cmd.empty() && (cmd[0] == 'g' || cmd[0] == 'G')) {
                                    try {
                                        size_t n = std::stoul(cmd.substr(1));
                                        if (n >= 1 && n <= work.size()) {
                                            cursorLine = n - 1;
                                            cursorCol = (std::min)(cursorCol, work[cursorLine].size());
                                        }
                                    }
                                    catch (...) {}
                                    drawWithCaret();
                                }
                                else { drawWithCaret(); }
                            }
                            else if (ch2 == 'q') {
                                if (dirty) {
                                    std::string choice = readCooked("Discard changes? (y/N): ");
                                    if (!choice.empty() && std::toupper((unsigned char)choice[0]) == 'Y') {
                                        dirty = false; break;
                                    }
                                    else { drawWithCaret(); }
                                }
                                else { break; }
                            }
                            else if (ch2 == 0x08 || ch2 == 0x7F) {
                                if (!work.empty() && cursorCol > 0) {
                                    std::string& s = work[cursorLine];
                                    s.erase(s.begin() + (long long)(cursorCol - 1));
                                    cursorCol--;
                                    dirty = true;
                                    drawWithCaret();
                                }
                            }
                            else if (ch2 >= 32 && ch2 < 127) {
                                if (work.empty()) work.emplace_back();
                                std::string& s = work[cursorLine];
                                if (cursorCol > s.size()) cursorCol = s.size();
                                if (insertMode || cursorCol == s.size()) s.insert(s.begin() + (long long)cursorCol, ch2);
                                else s[cursorCol] = ch2;
                                cursorCol++;
                                dirty = true;
                                drawWithCaret();
                            }
                        }
                    }
                }
                RenderActiveBufferPagedImpl(0, 0, gCfg.defaultSpacing);
                break;
            }


            default:
                std::cout << "[Unknown command: " << ch << "]\n";
                break;
            }
        }

        std::cout << "[OmniEditor] Closed.\n";
    }
}

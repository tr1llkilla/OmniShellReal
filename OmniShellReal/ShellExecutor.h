// =================================================================
// ShellExecutor.h
// Defines the interface for the process execution engine.
// UPDATED to support cross-platform implementation with Unicode
// and structured process results.
// =================================================================
#pragma once
#include <string>
#include <vector>
#include <map>

//
// Platform compatibility helpers for Windows builds
//
#ifdef _WIN32

// Reduce extraneous macros from Windows.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Target at least Windows 7 (0x0601). Raise if you need newer APIs.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <Windows.h>
#include <processenv.h>   // GetEnvironmentStringsW, FreeEnvironmentStringsW

#endif // _WIN32

// -----------------------------------------------------------------
// Supported scripting engines
// -----------------------------------------------------------------
enum class ScriptEngine {
    Auto,         // Detect by extension or shebang
    Cmd,          // Windows cmd.exe
    PowerShell,   // Windows PowerShell / pwsh
    Bash,         // bash/sh
    Python        // python/python3
};

// -----------------------------------------------------------------
// Options for local/remote script execution
// -----------------------------------------------------------------
struct ScriptOptions {
    std::string cwd;                      // Optional working directory
    std::map<std::string, std::string> env; // Environment overrides
    bool useLoginShell = false;           // Bash login shell (-l)
    bool captureStderr = true;            // Merge stderr into stdout
    std::string interpreterOverride;      // Custom interpreter path
};

// -----------------------------------------------------------------
// Target for remote execution
// -----------------------------------------------------------------
struct RemoteTarget {
    bool enabled = false;
    std::string host;       // "server" or "server:22" (ssh)
    std::string user;       // optional user
    int         port = 0;   // optional port (ssh)
};

// -----------------------------------------------------------------
// Structured process execution result (new API)
// -----------------------------------------------------------------
struct ExecResult {
    int exit_code = -1;       // Process exit code
    std::string stdout_data;  // Captured stdout
    std::string stderr_data;  // Captured stderr (if available separately)
};

// -----------------------------------------------------------------
// ShellExecutor
// -----------------------------------------------------------------
class ShellExecutor {
public:
    // ---------- EXISTING API ----------
    static std::string run(const std::string& cmd);
    static std::string runPowerShell(const std::string& cmd);

    // ---------- NEW: VS 2022 Developer Prompt API ----------
    static std::string runInDevPrompt(const std::string& cmd);

    static std::string compile(const std::string& src);

    // ---------- NEW HIGH-LEVEL SCRIPT APIS ----------
    static std::string runScriptInline(ScriptEngine engine,
        const std::string& code,
        const ScriptOptions& opt = {},
        const RemoteTarget& remote = {});

    static std::string runScriptFile(const std::string& scriptPath,
        const std::vector<std::string>& args = {},
        const ScriptOptions& opt = {},
        const RemoteTarget& remote = {});

    static std::string runPython(const std::string& codeOrFile,
        bool isFile = false,
        const std::vector<std::string>& args = {},
        const ScriptOptions& opt = {},
        const RemoteTarget& remote = {});

    static std::string runBash(const std::string& codeOrFile,
        bool isFile = false,
        const std::vector<std::string>& args = {},
        const ScriptOptions& opt = {},
        const RemoteTarget& remote = {});

    static ScriptEngine detectEngineByPath(const std::string& path);

    // ---------- NEW STRUCTURED EXECUTION API ----------
    // Full result capture: exit code + stdout + stderr
    static ExecResult executeStructured(const std::string& commandLine,
        const ScriptOptions& opt = {},
        const RemoteTarget& remote = {});
    // =================================================================
// NEW: Git and Vcpkg Command Helpers
// =================================================================
    static std::string gitClone(const std::string& repoUrl, const std::string& targetDir = "");
    static std::string vcpkgInstall(const std::string& packageName);
    static std::string vcpkgIntegrateInstall();

private:
#ifdef _WIN32
    // --- Legacy ANSI versions (kept for backward compatibility) ---
    static std::string executeProcess(const std::string& commandLine);
    static std::string executeProcess(const std::string& commandLine,
        const std::string& cwd,
        const std::map<std::string, std::string>& env,
        bool captureStderr);

    // --- Unicode-capable versions (new, safer for intl) ---
    static ExecResult executeProcessW(const std::wstring& commandLine,
        const std::wstring& cwd,
        const std::map<std::wstring, std::wstring>& env,
        bool captureStderr);
#endif
};
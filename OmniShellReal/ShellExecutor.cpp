Copyright © 2025 Cadell Richard Anderson

// =================================================================
// ShellExecutor.cpp
// =================================================================
#include "ShellExecutor.h"
#include <string>
#include <memory>
#include <array>
#include <iostream>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#include <processenv.h>   // GetEnvironmentStringsW, FreeEnvironmentStringsW
#else
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <algorithm> // For std::max
#endif

#ifdef _WIN32
// ------------------------------------------------------------
// REFACTORED Legacy process runners to use executeStructured
// ------------------------------------------------------------
std::string ShellExecutor::executeProcess(const std::string& commandLine) {
    ScriptOptions opt;
    opt.captureStderr = true; // Legacy function always merged stdout and stderr
    ExecResult result = executeStructured(commandLine, opt);
    return result.stdout_data;
}

std::string ShellExecutor::executeProcess(const std::string& commandLine,
    const std::string& cwd,
    const std::map<std::string, std::string>& env,
    bool captureStderr) {

    ScriptOptions opt;
    opt.cwd = cwd;
    opt.env = env;
    opt.captureStderr = captureStderr;

    ExecResult result = executeStructured(commandLine, opt);

    // The old function returned a single string, merging stderr if requested.
    // This logic is now handled by executeStructured.
    if (captureStderr) {
        return result.stdout_data;
    }
    return result.stdout_data; // If not capturing, return stdout only.
}
#endif // _WIN32

// ------------------------------------------------------------
// Simple command wrappers
// ------------------------------------------------------------
std::string ShellExecutor::run(const std::string& cmd) {
#if defined(_WIN32)
    return executeProcess("cmd.exe /c " + cmd);
#else
    // On POSIX, use the more robust executeStructured and mimic popen's behavior
    ScriptOptions opt;
    opt.captureStderr = true; // popen merges stderr
    ExecResult result = executeStructured(cmd, opt);
    return result.stdout_data;
#endif
}

std::string ShellExecutor::runPowerShell(const std::string& cmd) {
#if defined(_WIN32)
    return executeProcess("powershell.exe -NoProfile -Command \"" + cmd + "\"");
#else
    return run("pwsh -NoProfile -Command \"" + cmd + "\"");
#endif
}

std::string ShellExecutor::compile(const std::string& src) {
#if defined(_WIN32)
    // UPDATED: The compile command should run in the developer prompt.
    std::string command = "cl.exe /EHsc /Fe:\"" + src.substr(0, src.find_last_of('.')) + ".exe\" \"" + src + "\"";
    return runInDevPrompt(command);
#else
    std::string output_file = src.substr(0, src.find_last_of("."));
    std::string command = "g++ " + src + " -o " + output_file;
    return run(command);
#endif
}

// ======================================================================
// NEW: Logic for Visual Studio 2022 Developer Command Prompt
// ======================================================================
#ifdef _WIN32
namespace {
    // Helper to find the VsDevCmd.bat script for VS 2022.
    // This looks in the default installation directory. A more robust solution
    // could query the registry or use the vswhere utility.
    std::string findVsDevCmdPath() {
        // FIX C4996: Replaced unsafe std::getenv with _dupenv_s on Windows.
        char* vs_path_env_buf = nullptr;
        size_t vs_path_env_len = 0;
        errno_t err = _dupenv_s(&vs_path_env_buf, &vs_path_env_len, "VS2022INSTALLDIR");

        if (err == 0 && vs_path_env_buf != nullptr) {
            std::filesystem::path vs_path = vs_path_env_buf;
            free(vs_path_env_buf); // Important: free the buffer allocated by _dupenv_s
            vs_path /= "Common7\\Tools\\VsDevCmd.bat";
            if (std::filesystem::exists(vs_path)) {
                return vs_path.string();
            }
        }

        // Fallback to default location
        std::filesystem::path default_path = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\Tools\\VsDevCmd.bat";
        if (std::filesystem::exists(default_path)) {
            return default_path.string();
        }

        default_path = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\Common7\\Tools\\VsDevCmd.bat";
        if (std::filesystem::exists(default_path)) {
            return default_path.string();
        }

        default_path = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\Tools\\VsDevCmd.bat";
        if (std::filesystem::exists(default_path)) {
            return default_path.string();
        }

        return ""; // Not found
    }
}
#endif

// Executes a command within an initialized VS 2022 Developer Prompt.
std::string ShellExecutor::runInDevPrompt(const std::string& cmd) {
#if defined(_WIN32)
    static std::string vsDevCmdPath = findVsDevCmdPath();
    if (vsDevCmdPath.empty()) {
        return "[Error] Could not find VsDevCmd.bat for Visual Studio 2022.";
    }

    // Construct the command line to first run VsDevCmd.bat, then the user's command.
    // The quotes are important to handle spaces in paths.
    std::string full_command = "\"\"" + vsDevCmdPath + "\" && " + cmd + "\"";

    // Use the existing 'run' method which calls cmd.exe /c
    return run(full_command);
#else
    // On non-Windows platforms, this is a no-op that just runs the command directly.
    return run(cmd);
#endif
}
// ======================================================================
// NEW: Git and Vcpkg Helper Implementations
// ======================================================================

std::string ShellExecutor::gitClone(const std::string& repoUrl, const std::string& targetDir) {
    std::string command = "git clone " + repoUrl;
    if (!targetDir.empty()) {
        command += " \"" + targetDir + "\"";
    }
    // Use the structured executor to get full output, including errors
    ExecResult result = executeStructured(command);
    if (result.exit_code != 0) {
        return result.stdout_data + "\n" + result.stderr_data;
    }
    return result.stdout_data;
}

std::string ShellExecutor::vcpkgInstall(const std::string& packageName) {
    // Assumes vcpkg executable is in the system's PATH
    std::string command = "vcpkg install " + packageName;
    ExecResult result = executeStructured(command);
    if (result.exit_code != 0) {
        return result.stdout_data + "\n" + result.stderr_data;
    }
    return result.stdout_data;
}

std::string ShellExecutor::vcpkgIntegrateInstall() {
    // Assumes vcpkg executable is in the system's PATH
    std::string command = "vcpkg integrate install";
    ExecResult result = executeStructured(command);
    if (result.exit_code != 0) {
        return result.stdout_data + "\n" + result.stderr_data;
    }
    return result.stdout_data;
}

// ======================================================================
// Helpers
// ======================================================================
namespace {

    static inline std::string joinArgs(const std::vector<std::string>& v) {
        std::ostringstream oss;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) oss << " ";
#if defined(_WIN32)
            if (v[i].find(' ') != std::string::npos) oss << "\"" << v[i] << "\"";
            else oss << v[i];
#else
            if (v[i].find_first_of(" \t\"'`$&|;<>()") != std::string::npos) {
                oss << "'" << v[i] << "'";
            }
            else {
                oss << v[i];
            }
#endif
        }
        return oss.str();
    }

    static inline bool ends_with_ci(const std::string& s, const std::string& suff) {
        if (suff.size() > s.size()) return false;
        for (size_t i = 0; i < suff.size(); ++i) {
            char a = (char)std::tolower((unsigned char)s[s.size() - suff.size() + i]);
            char b = (char)std::tolower((unsigned char)suff[i]);
            if (a != b) return false;
        }
        return true;
    }

} // anon

// ======================================================================
// Engine detection
// ======================================================================
ScriptEngine ShellExecutor::detectEngineByPath(const std::string& path) {
    if (ends_with_ci(path, ".ps1")) return ScriptEngine::PowerShell;
#if defined(_WIN32)
    if (ends_with_ci(path, ".cmd") || ends_with_ci(path, ".bat")) return ScriptEngine::Cmd;
#endif
    if (ends_with_ci(path, ".sh")) return ScriptEngine::Bash;
    if (ends_with_ci(path, ".py")) return ScriptEngine::Python;
    return ScriptEngine::Auto;
}

// -----------------------------
// Core dispatch (inline script)
// -----------------------------
std::string ShellExecutor::runScriptInline(ScriptEngine engine,
    const std::string& code,
    const ScriptOptions& opt,
    const RemoteTarget& remote) {
    // Remote wrappers simply wrap the local command line construction
    auto build = [&](ScriptEngine e) -> std::string {
#if defined(_WIN32)
        switch (e) {
        case ScriptEngine::PowerShell: {
            std::string exe = opt.interpreterOverride.empty() ? "powershell.exe" : opt.interpreterOverride;
            return exe + " -NoProfile -ExecutionPolicy Bypass -Command \"" + code + "\"";
        }
        case ScriptEngine::Cmd: {
            return "cmd.exe /c " + code;
        }
        case ScriptEngine::Python: {
            std::string exe = opt.interpreterOverride.empty() ? "python" : opt.interpreterOverride;
            return exe + " -c \"" + code + "\"";
        }
        case ScriptEngine::Bash: {
            std::string exe = opt.interpreterOverride.empty() ? "bash" : opt.interpreterOverride;
            std::string login = opt.useLoginShell ? " -l" : "";
            return exe + login + " -c " + "\"" + code + "\"";
        }
        default:
            return code;
        }
#else
        switch (e) {
        case ScriptEngine::PowerShell: {
            std::string exe = opt.interpreterOverride.empty() ? "pwsh" : opt.interpreterOverride;
            return exe + " -NoProfile -Command \"" + code + "\"";
        }
        case ScriptEngine::Python: {
            std::string exe = opt.interpreterOverride.empty() ? "python3" : opt.interpreterOverride;
            return exe + " -c \"" + code + "\"";
        }
        case ScriptEngine::Bash: {
            std::string exe = opt.interpreterOverride.empty() ? "/bin/bash" : opt.interpreterOverride;
            std::string login = opt.useLoginShell ? " -l" : "";
            return exe + login + " -c \"" + code + "\"";
        }
        default: {
            return "/bin/sh -c \"" + code + "\"";
        }
        }
#endif
        };

    // Local or remote?
    if (!remote.enabled) {
        std::string command = build(engine == ScriptEngine::Auto ?
#ifdef _WIN32
            ScriptEngine::Cmd
#else
            ScriptEngine::Bash
#endif
            : engine);
        ExecResult result = executeStructured(command, opt, remote);
        return opt.captureStderr ? result.stdout_data + result.stderr_data : result.stdout_data;
    }

    // -------- Remote wrappers --------
#if defined(_WIN32)
    // Use PowerShell remoting (WinRM)
    std::ostringstream cmd;
    std::string psScript;
    if (engine == ScriptEngine::PowerShell || engine == ScriptEngine::Auto) {
        psScript = code;
    }
    else {
        if (engine == ScriptEngine::Bash) {
            psScript = "bash -lc \\\"" + code + "\\\"";
        }
        else if (engine == ScriptEngine::Python) {
            psScript = "python -c \\\"" + code + "\\\"";
        }
        else {
            psScript = code;
        }
    }
    cmd << "powershell.exe -NoProfile -Command "
        << "\"Invoke-Command -ComputerName '" << remote.host << "'"
        << (remote.user.empty() ? "" : " -Credential (Get-Credential)")
        << " -ScriptBlock {";
    if (!opt.cwd.empty()) cmd << "Set-Location -Path '" << opt.cwd << "'; ";
    if (!opt.env.empty()) {
        for (auto& kv : opt.env) cmd << "$Env:" << kv.first << "='" << kv.second << "'; ";
    }
    cmd << psScript << "}\"";
    return executeProcess(cmd.str(), "", {}, opt.captureStderr);
#else
    // SSH
    std::ostringstream ssh;
    std::string userhost = remote.user.empty() ? remote.host : (remote.user + "@" + remote.host);
    ssh << "ssh ";
    if (remote.port > 0) ssh << "-p " << remote.port << " ";
    ssh << userhost << " ";
    std::ostringstream r;
    if (!opt.cwd.empty()) r << "cd " << "'" << opt.cwd << "'" << " && ";
    for (auto& kv : opt.env) r << kv.first << "=" << "'" << kv.second << "'" << " ";
    r << build(engine == ScriptEngine::Auto ? ScriptEngine::Bash : engine);
    std::string remoteCmd = r.str();
    ssh << "'" << remoteCmd << "'";
    return run(ssh.str());
#endif
}

// -----------------------------
// Script file execution
// -----------------------------
std::string ShellExecutor::runScriptFile(const std::string& scriptPath,
    const std::vector<std::string>& args,
    const ScriptOptions& opt,
    const RemoteTarget& remote) {
    ScriptEngine eng = detectEngineByPath(scriptPath);
    auto buildFileCmd = [&](ScriptEngine e) -> std::string {
        std::string argv = joinArgs(args);
#if defined(_WIN32)
        switch (e) {
        case ScriptEngine::PowerShell: {
            std::string exe = opt.interpreterOverride.empty() ? "powershell.exe" : opt.interpreterOverride;
            return exe + " -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\" " + argv;
        }
        case ScriptEngine::Cmd: {
            return "cmd.exe /c \"" + scriptPath + (argv.empty() ? "" : (" " + argv)) + "\"";
        }
        case ScriptEngine::Python: {
            std::string exe = opt.interpreterOverride.empty() ? "python" : opt.interpreterOverride;
            return exe + " \"" + scriptPath + "\" " + argv;
        }
        case ScriptEngine::Bash: {
            std::string exe = opt.interpreterOverride.empty() ? "bash" : opt.interpreterOverride;
            std::string login = opt.useLoginShell ? " -l" : "";
            return exe + login + " \"" + scriptPath + "\" " + argv;
        }
        default:
            return "cmd.exe /c \"" + scriptPath + (argv.empty() ? "" : (" " + argv)) + "\"";
        }
#else
        switch (e) {
        case ScriptEngine::PowerShell: {
            std::string exe = opt.interpreterOverride.empty() ? "pwsh" : opt.interpreterOverride;
            return exe + " -NoProfile -File \"" + scriptPath + "\" " + argv;
        }
        case ScriptEngine::Python: {
            std::string exe = opt.interpreterOverride.empty() ? "python3" : opt.interpreterOverride;
            return exe + " \"" + scriptPath + "\" " + argv;
        }
        case ScriptEngine::Bash: {
            std::string exe = opt.interpreterOverride.empty() ? "/bin/bash" : opt.interpreterOverride;
            std::string login = opt.useLoginShell ? " -l" : "";
            return exe + login + " \"" + scriptPath + "\" " + argv;
        }
        default: {
            return "/bin/sh \"" + scriptPath + "\" " + argv;
        }
        }
#endif
        };

    if (!remote.enabled) {
        std::string command = buildFileCmd(eng);
        ExecResult result = executeStructured(command, opt, remote);
        return opt.captureStderr ? result.stdout_data + result.stderr_data : result.stdout_data;
    }

    std::ostringstream invoke;
    invoke << "'" << scriptPath << "' " << joinArgs(args);
    return runScriptInline(eng, invoke.str(), opt, remote);
}

// -----------------------------
// Convenience helpers
// -----------------------------
std::string ShellExecutor::runPython(const std::string& codeOrFile,
    bool isFile,
    const std::vector<std::string>& args,
    const ScriptOptions& opt,
    const RemoteTarget& remote) {
    return isFile
        ? runScriptFile(codeOrFile, args, opt, remote)
        : runScriptInline(ScriptEngine::Python, codeOrFile, opt, remote);
}

std::string ShellExecutor::runBash(const std::string& codeOrFile,
    bool isFile,
    const std::vector<std::string>& args,
    const ScriptOptions& opt,
    const RemoteTarget& remote) {
    return isFile
        ? runScriptFile(codeOrFile, args, opt, remote)
        : runScriptInline(ScriptEngine::Bash, codeOrFile, opt, remote);
}

// ======================================================================
// NEW STRUCTURED EXECUTION API IMPLEMENTATION
// ======================================================================

#ifdef _WIN32

namespace {
    std::wstring to_wstring(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
        std::wstring wstrTo(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
        return wstrTo;
    }
} // anonymous namespace

ExecResult ShellExecutor::executeProcessW(const std::wstring& commandLine,
    const std::wstring& cwd,
    const std::map<std::wstring, std::wstring>& env,
    bool captureStderr) {

    ExecResult result;
    result.exit_code = -1;
    std::wstring mutableCmd = commandLine;

    HANDLE hStdOutRead = NULL, hStdOutWrite = NULL;
    HANDLE hStdErrRead = NULL, hStdErrWrite = NULL;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
        result.stderr_data = "Error: Could not create stdout pipe.";
        return result;
    }
    if (!SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
        result.stderr_data = "Error: Could not set handle information for stdout.";
        return result;
    }

    if (!captureStderr) {
        if (!CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0)) {
            CloseHandle(hStdOutRead);
            CloseHandle(hStdOutWrite);
            result.stderr_data = "Error: Could not create stderr pipe.";
            return result;
        }
        if (!SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0)) {
            CloseHandle(hStdOutRead);
            CloseHandle(hStdOutWrite);
            CloseHandle(hStdErrRead);
            CloseHandle(hStdErrWrite);
            result.stderr_data = "Error: Could not set handle information for stderr.";
            return result;
        }
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.hStdOutput = hStdOutWrite;
    si.hStdError = captureStderr ? hStdOutWrite : hStdErrWrite;
    si.dwFlags |= STARTF_USESTDHANDLES;
    ZeroMemory(&pi, sizeof(pi));

    std::vector<wchar_t> envBlock;
    LPWCH lpvEnv = NULL;
    if (!env.empty()) {
        wchar_t* currentEnv = GetEnvironmentStringsW();
        std::map<std::wstring, std::wstring> mergedEnv;
        for (wchar_t* p = currentEnv; *p; ) {
            std::wstring kv = p;
            p += kv.size() + 1;
            size_t eq_pos = kv.find(L'=');
            if (eq_pos != std::wstring::npos) {
                mergedEnv[kv.substr(0, eq_pos)] = kv.substr(eq_pos + 1);
            }
        }
        FreeEnvironmentStringsW(currentEnv);
        for (const auto& pair : env) mergedEnv[pair.first] = pair.second;

        for (const auto& pair : mergedEnv) {
            envBlock.insert(envBlock.end(), pair.first.begin(), pair.first.end());
            envBlock.push_back(L'=');
            envBlock.insert(envBlock.end(), pair.second.begin(), pair.second.end());
            envBlock.push_back(L'\0');
        }
        envBlock.push_back(L'\0');
        lpvEnv = envBlock.data();
    }


    if (!CreateProcessW(NULL, &mutableCmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, lpvEnv, cwd.empty() ? NULL : cwd.c_str(), &si, &pi)) {
        CloseHandle(hStdOutRead);
        CloseHandle(hStdOutWrite);
        if (!captureStderr) {
            CloseHandle(hStdErrRead);
            CloseHandle(hStdErrWrite);
        }
        result.stderr_data = "Error: CreateProcessW failed.";
        return result;
    }

    CloseHandle(hStdOutWrite);
    if (!captureStderr) CloseHandle(hStdErrWrite);

    std::array<char, 256> buffer{};
    DWORD dwRead;

    while (ReadFile(hStdOutRead, buffer.data(), static_cast<DWORD>(buffer.size()), &dwRead, NULL) && dwRead != 0) {
        result.stdout_data.append(buffer.data(), dwRead);
    }

    if (!captureStderr) {
        while (ReadFile(hStdErrRead, buffer.data(), static_cast<DWORD>(buffer.size()), &dwRead, NULL) && dwRead != 0) {
            result.stderr_data.append(buffer.data(), dwRead);
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exit_code = static_cast<int>(exitCode);

    CloseHandle(hStdOutRead);
    if (!captureStderr) CloseHandle(hStdErrRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return result;
}

#else // POSIX implementation

namespace {
    ExecResult executeProcessPosix(const std::string& commandLine,
        const std::string& cwd,
        const std::map<std::string, std::string>& env,
        bool captureStderr) {

        ExecResult result;
        result.exit_code = -1;

        // FIX: Initialize local variables per lnt-uninitialized-local warning
        int stdout_pipe[2]{}, stderr_pipe[2]{};

        if (pipe(stdout_pipe) < 0 || (!captureStderr && pipe(stderr_pipe) < 0)) {
            result.stderr_data = "Error: Could not create pipe.";
            return result;
        }

        pid_t pid = fork();

        if (pid < 0) { // Fork failed
            result.stderr_data = "Error: Fork failed.";
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
            if (!captureStderr) {
                close(stderr_pipe[0]);
                close(stderr_pipe[1]);
            }
            return result;
        }

        if (pid == 0) { // Child process
            close(stdout_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);

            if (captureStderr) {
                dup2(stdout_pipe[1], STDERR_FILENO);
            }
            else {
                close(stderr_pipe[0]);
                dup2(stderr_pipe[1], STDERR_FILENO);
                close(stderr_pipe[1]);
            }
            close(stdout_pipe[1]);

            if (!cwd.empty()) chdir(cwd.c_str());
            for (const auto& pair : env) setenv(pair.first.c_str(), pair.second.c_str(), 1);

            execl("/bin/sh", "sh", "-c", commandLine.c_str(), (char*)NULL);
            exit(127);
        }
        else { // Parent process
            close(stdout_pipe[1]);
            if (!captureStderr) close(stderr_pipe[1]);

            fd_set read_fds;
            int stdout_fd = stdout_pipe[0];
            int stderr_fd = captureStderr ? -1 : stderr_pipe[0];
            int max_fd = std::max(stdout_fd, stderr_fd) + 1;

            std::array<char, 256> buffer;

            while (stdout_fd != -1 || stderr_fd != -1) {
                FD_ZERO(&read_fds);
                if (stdout_fd != -1) FD_SET(stdout_fd, &read_fds);
                if (stderr_fd != -1) FD_SET(stderr_fd, &read_fds);

                if (select(max_fd, &read_fds, NULL, NULL, NULL) < 0) break;

                if (stdout_fd != -1 && FD_ISSET(stdout_fd, &read_fds)) {
                    ssize_t count = read(stdout_fd, buffer.data(), buffer.size());
                    if (count > 0) result.stdout_data.append(buffer.data(), count);
                    else {
                        close(stdout_fd);
                        stdout_fd = -1;
                    }
                }
                if (stderr_fd != -1 && FD_ISSET(stderr_fd, &read_fds)) {
                    ssize_t count = read(stderr_fd, buffer.data(), buffer.size());
                    if (count > 0) result.stderr_data.append(buffer.data(), count);
                    else {
                        close(stderr_fd);
                        stderr_fd = -1;
                    }
                }
            }

            // FIX: Initialize local variable per lnt-uninitialized-local warning
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
        }
        return result;
    }
} // anonymous namespace

#endif


// Full result capture: exit code + stdout + stderr
ExecResult ShellExecutor::executeStructured(const std::string& commandLine,
    const ScriptOptions& opt,
    const RemoteTarget& remote) {

    if (remote.enabled) {
        ExecResult result;
        result.exit_code = -1;
        result.stderr_data = "Remote execution is not supported by the executeStructured API.";
        return result;
    }

#ifdef _WIN32
    std::wstring wCommandLine = to_wstring(commandLine);
    std::wstring wCwd = to_wstring(opt.cwd);
    std::map<std::wstring, std::wstring> wEnv;
    for (const auto& pair : opt.env) {
        wEnv[to_wstring(pair.first)] = to_wstring(pair.second);
    }
    return executeProcessW(wCommandLine, wCwd, wEnv, opt.captureStderr);
#else
    return executeProcessPosix(commandLine, opt.cwd, opt.env, opt.captureStderr);
#endif
}

// ============================================================================
// End of ShellExecutor.cpp
// ============================================================================

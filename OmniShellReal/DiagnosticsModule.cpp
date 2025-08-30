// =================================================================
// DiagnosticsModule.cpp
// UPDATED with concurrent, intelligent AI analysis for high-entropy files.
// =================================================================
#include "DiagnosticsModule.h"
#include "ShellExecutor.h"
#include "BinaryTranslator.h"
#include "JobManager.h"
#include "OmniEditorIDE.h"
#include "OmniAIManager.h" // Include for AI summarization

#include <iostream>
#include <sstream>
#include <unordered_map>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <algorithm>

#ifdef _WIN32
#include <tlhelp32.h>
#include <psapi.h>

// Helper function to convert wstring to string
std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}
#endif

#ifdef _WIN32
bool DiagnosticsModule::ContainsSubstring(const std::wstring& str, const std::wstring& substr) {
    return str.find(substr) != std::wstring::npos;
}

HKEY DiagnosticsModule::GetRootKey(const std::string& rootKeyStr) {
    if (rootKeyStr == "HKLM") return HKEY_LOCAL_MACHINE;
    if (rootKeyStr == "HKCU") return HKEY_CURRENT_USER;
    if (rootKeyStr == "HKCR") return HKEY_CLASSES_ROOT;
    if (rootKeyStr == "HKU") return HKEY_USERS;
    return NULL;
}

void DiagnosticsModule::SearchRegistryRecursive(HKEY hKey, const std::wstring& searchTerm, std::vector<std::wstring>& foundItems) {
    TCHAR    achKey[255];
    DWORD    cbName;
    TCHAR    achClass[MAX_PATH] = TEXT("");
    DWORD    cchClassName = MAX_PATH;
    DWORD    cSubKeys = 0;
    DWORD    cbMaxSubKey;
    DWORD    cchMaxClass;
    DWORD    cValues;
    DWORD    cchMaxValue;
    DWORD    cbMaxValueData;
    DWORD    cbSecurityDescriptor;
    FILETIME ftLastWriteTime;

    RegQueryInfoKey(hKey, achClass, &cchClassName, NULL, &cSubKeys, &cbMaxSubKey, &cchMaxClass, &cValues, &cchMaxValue, &cbMaxValueData, &cbSecurityDescriptor, &ftLastWriteTime);

    if (cSubKeys) {
        for (DWORD i = 0; i < cSubKeys; i++) {
            cbName = 255;
            if (RegEnumKeyEx(hKey, i, achKey, &cbName, NULL, NULL, NULL, &ftLastWriteTime) == ERROR_SUCCESS) {
                if (ContainsSubstring(achKey, searchTerm)) {
                    foundItems.push_back(achKey);
                }
            }
        }
    }
}

std::string DiagnosticsModule::ScanRegistry(const std::string& rootKeyStr, const std::vector<std::string>& searchTerms) {
    HKEY hRootKey = GetRootKey(rootKeyStr);
    if (!hRootKey) {
        return "Error: Invalid root key specified. Use HKLM, HKCU, HKCR, or HKU.";
    }

    std::vector<std::wstring> foundItems;
    for (const auto& term : searchTerms) {
        std::wstring wSearchTerm(term.begin(), term.end());
        SearchRegistryRecursive(hRootKey, wSearchTerm, foundItems);
    }

    std::stringstream ss;
    ss << "--- Registry Scan Results ---\n";
    if (foundItems.empty()) {
        ss << "No items found matching search terms.\n";
    }
    else {
        for (const auto& item : foundItems) {
            ss << wstring_to_string(item) << "\n";
        }
    }
    return ss.str();
}
#else
std::string DiagnosticsModule::ScanRegistry(const std::string&, const std::vector<std::string>&) {
    return "Registry scanning is only available on Windows.";
}
#endif

double DiagnosticsModule::calculateEntropy(const std::vector<unsigned char>& data) {
    if (data.empty()) return 0.0;
    std::unordered_map<unsigned char, size_t> freq;
    for (unsigned char byte : data) freq[byte]++;

    double entropy = 0.0;
    for (const auto& pair : freq) {
        double p = static_cast<double>(pair.second) / data.size();
        entropy -= p * std::log2(p);
    }
    return entropy;
}

// ======================= NEW: Signature Matching =======================
bool DiagnosticsModule::matchSignatures(const std::vector<unsigned char>& data, std::string& matchedSigName) {
    static const std::vector<std::pair<std::string, std::vector<unsigned char>>> signatures = {
        { "MZ_Header", { 'M', 'Z' } },
        { "UPX_Packer", { 'U', 'P', 'X', '!' } },
        { "ELF_Header", { 0x7F, 'E', 'L', 'F' } },
        { "Malicious_DLL_Load", { 'L','o','a','d','L','i','b','r','a','r','y','A' } },
        { "Suspicious_PowerShell", { 'P','o','w','e','r','S','h','e','l','l' } }
    };

    for (const auto& sig : signatures) {
        auto it = std::search(data.begin(), data.end(), sig.second.begin(), sig.second.end());
        if (it != data.end()) {
            matchedSigName = sig.first;
            return true;
        }
    }
    return false;
}
// =======================================================================

std::string DiagnosticsModule::analyzeAndReport(const std::string& filePath, const std::string& reportDir) {
    JobManager::SubmitJob([filePath, reportDir]() {
        std::string filename = std::filesystem::path(filePath).filename().string();

        std::string decompiledCode = BinaryTranslator::Decompile(filePath);

        std::string summary = OmniAIManager::summarize(decompiledCode);

        std::ostringstream reportContent;
        reportContent << "AI Analysis for: " << filename << "\n";
        reportContent << "======================================\n\n";
        reportContent << "--- AI Summary ---\n" << summary << "\n\n";
        reportContent << "--- Heuristic Detections ---\n";
        if (decompiledCode.find("CreateRemoteThread") != std::string::npos) reportContent << "[!] Code Injection Detected\n";
        if (decompiledCode.find("GetProcAddress") != std::string::npos) reportContent << "[!] Dynamic API Resolution Detected\n";
        if (decompiledCode.find("WriteProcessMemory") != std::string::npos) reportContent << "[!] Memory Tampering Detected\n";
        if (decompiledCode.find("socket") != std::string::npos || decompiledCode.find("WSASocket") != std::string::npos) reportContent << "[!] Network Activity Detected\n";
        if (decompiledCode.find("RegCreateKey") != std::string::npos || decompiledCode.find("RegSetValue") != std::string::npos) reportContent << "[!] Registry Manipulation Detected\n";

        try {
            std::filesystem::create_directories(reportDir);
            std::ofstream reportFile(std::filesystem::path(reportDir) / (filename + "_analysis.txt"));
            reportFile << reportContent.str();
            reportFile.close();

            OmniEditorIDE::OpenBuffer(filename + "_analysis.txt", reportContent.str());
        }
        catch (const std::exception& e) {
            std::cerr << "Error writing report: " << e.what() << "\n";
        }
        });
    return "Analysis job for " + std::filesystem::path(filePath).filename().string() + " submitted to background queue.";
}

std::string DiagnosticsModule::ScanFileEntropy(const std::string& path, const std::string& quarantineDir, const std::string& reportDir, double entropyThreshold) {
    std::stringstream ss;
    if (!std::filesystem::exists(path)) {
        return "Error: Path does not exist.";
    }

    try {
        auto scan_file = [&](const std::filesystem::path& filePath) {
            std::ifstream file(filePath, std::ios::binary);
            if (!file) return;
            std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(file), {});

            // ===== Signature detection before entropy check =====
            std::string matchedSig;
            if (matchSignatures(buffer, matchedSig)) {
                ss << "  - " << filePath.string() << ": [SIGNATURE MATCH: " << matchedSig << "]";
                try {
                    std::filesystem::create_directories(quarantineDir);
                    std::filesystem::copy(filePath, std::filesystem::path(quarantineDir) / filePath.filename(), std::filesystem::copy_options::overwrite_existing);
                    ss << " -> Quarantined due to signature.\n";
                    ss << "    " << analyzeAndReport(filePath.string(), reportDir) << "\n";
                }
                catch (const std::exception& e) {
                    ss << " -> Failed to quarantine: " << e.what() << "\n";
                }
                return; // Skip entropy check if signature found
            }
            // =====================================================

            double entropy = calculateEntropy(buffer);
            ss << "  - " << filePath.string() << ": " << std::fixed << std::setprecision(4) << entropy;
            if (entropy > entropyThreshold) {
                ss << " [HIGH ENTROPY DETECTED]";
                try {
                    std::filesystem::create_directories(quarantineDir);
                    std::filesystem::copy(filePath, std::filesystem::path(quarantineDir) / filePath.filename(), std::filesystem::copy_options::overwrite_existing);
                    ss << " -> Quarantined.\n";
                    ss << "    " << analyzeAndReport(filePath.string(), reportDir) << "\n";
                }
                catch (const std::exception& e) {
                    ss << " -> Failed to quarantine: " << e.what() << "\n";
                }
            }
            else {
                ss << "\n";
            }
            };

        if (std::filesystem::is_regular_file(path)) {
            scan_file(path);
        }
        else if (std::filesystem::is_directory(path)) {
            ss << "Scanning directory: " << path << "\n";
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file()) {
                    scan_file(entry.path());
                }
            }
        }
    }
    catch (const std::exception& e) {
        ss << "An error occurred: " << e.what() << "\n";
    }
    return ss.str();
}

#ifdef _WIN32
std::string DiagnosticsModule::MonitorProcesses() {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return "Error creating process snapshot.";
    }

    PROCESSENTRY32 processEntry;
    processEntry.dwSize = sizeof(PROCESSENTRY32);
    std::stringstream ss;
    ss << "--- Running Processes ---\n";
    ss << std::left << std::setw(10) << "PID" << std::setw(30) << "Process Name" << "\n";

    if (Process32First(hSnapshot, &processEntry)) {
        do {
            std::string processName = wstring_to_string(processEntry.szExeFile);
            ss << std::left << std::setw(10) << processEntry.th32ProcessID << std::setw(30) << processName << "\n";
        } while (Process32Next(hSnapshot, &processEntry));
    }
    CloseHandle(hSnapshot);
    return ss.str();
}

std::string DiagnosticsModule::TerminateProcessByPID(unsigned long pid) {
    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!hProcess) {
        return "Error: Unable to open process with PID: " + std::to_string(pid);
    }

    if (TerminateProcess(hProcess, 0)) {
        CloseHandle(hProcess);
        return "Process with PID " + std::to_string(pid) + " has been terminated.";
    }
    else {
        CloseHandle(hProcess);
        return "Error: Failed to terminate process with PID " + std::to_string(pid);
    }
}

#else
std::string DiagnosticsModule::MonitorProcesses() {
    return ShellExecutor::run("ps aux");
}
std::string DiagnosticsModule::TerminateProcessByPID(unsigned long pid) {
    return ShellExecutor::run("kill " + std::to_string(pid));
}
#endif

void DiagnosticsModule::AnalyzeBinary(const std::string& filepath) {
    JobManager::SubmitJob([filepath]() {
        std::string result = BinaryTranslator::Decompile(filepath);
        std::filesystem::create_directory("reports");
        std::ofstream out("reports/" + std::filesystem::path(filepath).filename().string() + ".txt");
        out << result;
        out.close();
        OmniEditorIDE::OpenBuffer(filepath, result);
        OmniEditorIDE::LaunchInteractiveUI();
        });
}

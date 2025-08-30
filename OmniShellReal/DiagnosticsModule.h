// =================================================================
// DiagnosticsModule.h
// =================================================================
#pragma once
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

class DiagnosticsModule {
public:
    static std::string ScanRegistry(const std::string& rootKey, const std::vector<std::string>& searchTerms);
    static std::string ScanFileEntropy(const std::string& path, const std::string& quarantineDir, const std::string& reportDir, double entropyThreshold);
    static std::string MonitorProcesses();
    static std::string TerminateProcessByPID(unsigned long pid);
    static void AnalyzeBinary(const std::string& filepath);

private:
#ifdef _WIN32
    static void SearchRegistryRecursive(HKEY hKey, const std::wstring& searchTerm, std::vector<std::wstring>& foundItems);
    static bool ContainsSubstring(const std::wstring& str, const std::wstring& substr);
    static HKEY GetRootKey(const std::string& rootKeyStr);
#endif
    static double calculateEntropy(const std::vector<unsigned char>& data);
    static std::string analyzeAndReport(const std::string& filePath, const std::string& reportDir);

    // NEW: Lightweight signature matcher for detecting known malicious patterns
    static bool matchSignatures(const std::vector<unsigned char>& data, std::string& matchedSigName);
};

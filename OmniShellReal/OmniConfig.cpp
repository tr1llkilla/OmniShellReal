Copyright © 2025 Cadell Richard Anderson

//OmniConfig.cpp

#include "OmniConfig.h"
#include "tinyxml2.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace OmniConfigNS {

    bool load(const std::string& configFilePath, ConfigState& config) {
        tinyxml2::XMLDocument doc; // fully qualified to avoid ambiguity

        if (doc.LoadFile(configFilePath.c_str()) != tinyxml2::XML_SUCCESS) {
            std::cerr << "[Warning] Could not load " << configFilePath
                << ". Using default settings." << std::endl;
            return false;
        }

        tinyxml2::XMLElement* root = doc.FirstChildElement("OmniShell");
        if (!root) {
            std::cerr << "[Warning] Malformed OmniConfig.xml. Using default settings."
                << std::endl;
            return false;
        }

        if (tinyxml2::XMLElement* elem = root->FirstChildElement("EnableDiskLog")) {
            elem->QueryBoolText(&config.enableDiskLog);
        }
        if (tinyxml2::XMLElement* elem = root->FirstChildElement("AllowRoot")) {
            elem->QueryBoolText(&config.allowRoot);
        }
        if (tinyxml2::XMLElement* elem = root->FirstChildElement("MonitorSensors")) {
            elem->QueryBoolText(&config.monitorSensors);
        }

        if (tinyxml2::XMLElement* thresholds = root->FirstChildElement("SensorThresholds")) {
            if (tinyxml2::XMLElement* cpuElem = thresholds->FirstChildElement("CPU")) {
                cpuElem->QueryIntText(&config.cpuThreshold);
            }
            if (tinyxml2::XMLElement* batteryElem = thresholds->FirstChildElement("BatteryMin")) {
                batteryElem->QueryIntText(&config.batteryMinThreshold);
            }
        }

        // Optional: override entropyThreshold if present
        if (tinyxml2::XMLElement* elem = root->FirstChildElement("EntropyThreshold")) {
            elem->QueryDoubleText(&config.entropyThreshold);
        }

        // NEW: parse signatures
        if (tinyxml2::XMLElement* sigRoot = root->FirstChildElement("Signatures")) {
            for (tinyxml2::XMLElement* sigElem = sigRoot->FirstChildElement("Signature");
                sigElem != nullptr;
                sigElem = sigElem->NextSiblingElement("Signature"))
            {
                const char* nameAttr = sigElem->Attribute("name");
                const char* hexPattern = sigElem->GetText();

                if (nameAttr && hexPattern) {
                    std::vector<unsigned char> patternBytes;
                    std::istringstream hexStream(hexPattern);
                    std::string byteStr;

                    while (hexStream >> byteStr) {
                        while (!byteStr.empty() &&
                            !std::isxdigit(static_cast<unsigned char>(byteStr.back()))) {
                            byteStr.pop_back();
                        }
                        if (byteStr.empty()) continue;

                        unsigned int byteVal = 0;
                        std::stringstream ss;
                        ss << std::hex << byteStr;
                        ss >> byteVal;
                        patternBytes.push_back(static_cast<unsigned char>(byteVal & 0xFFu));
                    }

                    config.signaturePatterns.emplace_back(
                        std::string(nameAttr), std::move(patternBytes));
                }
            }
        }

        std::cout << "[*] Config loaded from: " << configFilePath << std::endl;
        return true;
    }

    // --- helper: get exe dir ---
    static std::filesystem::path get_exe_dir() {
#if defined(_WIN32)
        wchar_t buf[MAX_PATH]{ 0 };
        DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        if (len > 0) {
            return std::filesystem::path(buf).parent_path();
        }
#endif
        return std::filesystem::current_path();
    }

    // --- platform-safe getenv wrapper ---
    static std::string get_env_str(const char* var) {
#if defined(_WIN32)
        char* buf = nullptr;
        size_t len = 0;
        if (_dupenv_s(&buf, &len, var) == 0 && buf) {
            std::string val(buf);
            free(buf);
            return val;
        }
        return {};
#else
        const char* v = std::getenv(var);
        return v ? std::string(v) : std::string{};
#endif
    }

    // --- NEW: load_with_fallback ---
    bool load_with_fallback(ConfigState& outConfig, const std::string& explicitPath) {
        std::vector<std::filesystem::path> candidates;

        // 1) explicit arg
        if (!explicitPath.empty())
            candidates.emplace_back(explicitPath);

        // 2) env override
        if (auto envPath = get_env_str("OMNI_CONFIG"); !envPath.empty()) {
            candidates.emplace_back(envPath);
        }

        // 3) known absolute path
        candidates.emplace_back(R"(PATH)");

        // 4) exe-relative
        auto exeDir = get_exe_dir();
        candidates.emplace_back(exeDir / "OmniConfig.xml");
        candidates.emplace_back(exeDir / "config" / "OmniConfig.xml");

        // 5) working dir
        candidates.emplace_back(std::filesystem::current_path() / "OmniConfig.xml");

        for (const auto& c : candidates) {
            std::error_code ec;
            if (!std::filesystem::exists(c, ec)) continue;
            if (OmniConfigNS::load(c.string(), outConfig)) {
                return true;
            }
        }

        // fallback to original relative call
        return OmniConfigNS::load("OmniConfig.xml", outConfig);
    }

} // namespace OmniConfigNS

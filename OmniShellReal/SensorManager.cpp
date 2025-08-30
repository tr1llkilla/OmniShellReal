Copyright © 2025 Cadell Richard Anderson

// =================================================================
// SensorManager.cpp
// =================================================================
#include "SensorManager.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <numeric>
#include <chrono>
#include <thread>
#include <algorithm>
#include <map>
#include <unordered_set>
#include <functional>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#include <Wbemidl.h>
#include <comdef.h>
#pragma comment(lib, "wbemuuid.lib")

// Helper function to convert BSTR to std::string
static std::string bstr_to_str(BSTR bstr) {
    if (!bstr) return "";
    _bstr_t b(bstr, false);
    return std::string(b);
}
#else
#include <sys/statvfs.h>
#endif

// Helper for Linux CPU calculation
#if defined(__linux__)
struct CpuTimes {
    long long user = 0, nice = 0, system = 0, idle = 0;
    long long iowait = 0, irq = 0, softirq = 0, steal = 0;

    long long getTotal() const {
        return user + nice + system + idle + iowait + irq + softirq + steal;
    }
    long long getIdle() const {
        return idle + iowait;
    }
};

static CpuTimes getCpuTimes() {
    std::ifstream proc_stat("/proc/stat");
    proc_stat.ignore(5, ' '); // Skip "cpu"
    CpuTimes t;
    proc_stat >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >> t.steal;
    return t;
}
#endif

// ---------------------
// Additive helper utilities (do not remove existing code above)
// ---------------------

// Generic safe sysfs reader
static std::string readSysfsValue(const std::filesystem::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return "";
    std::string s;
    std::getline(f, s);
    // trim
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

// Maintain a set of seen sensor IDs to avoid duplicates when adding additive scans
static void addSensorIfNew(std::vector<SensorData>& sensors, const SensorData& sd, std::unordered_set<std::string>& seen) {
    if (sd.id.empty()) {
        // fallback compose id from type+name
        std::string fallback = sd.type + "_" + sd.name;
        if (!fallback.empty() && seen.find(fallback) == seen.end()) {
            sensors.push_back(sd);
            seen.insert(fallback);
        }
        return;
    }
    if (seen.find(sd.id) == seen.end()) {
        sensors.push_back(sd);
        seen.insert(sd.id);
    }
}

// Linux: exhaustive hwmon parser - safe, additive (calls should be made after your existing hwmon scan)
#if defined(__linux__)
static void parseHwmonDevice(const std::filesystem::path& hwmonPath, std::vector<SensorData>& sensors, std::unordered_set<std::string>& seen) {
    // read chip name
    std::string chipName = readSysfsValue(hwmonPath / "name");
    if (chipName.empty()) chipName = hwmonPath.filename().string();

    auto pushFromAttr = [&](const std::string& attrPath, const std::string& idPrefix,
        const std::string& type, const std::string& labelHint,
        const std::string& units,
        std::function<double(long long)> transform = nullptr,
        std::function<SensorStatus(double)> statusEval = nullptr) {
            std::filesystem::path p = hwmonPath / attrPath;
            if (!std::filesystem::exists(p)) return;
            std::string raw = readSysfsValue(p);
            if (raw.empty()) return;
            try {
                long long vi = std::stoll(raw);
                double v = transform ? transform(vi) : static_cast<double>(vi);
                SensorStatus st = SensorStatus::OK;
                if (statusEval) st = statusEval(v);
                std::string label = labelHint;
                // attempt to load label file e.g. temp1_label or in0_label
                std::string labelFile = attrPath;
                // attrPath often like "temp1_input" -> label name "temp1_label"
                size_t pos = labelFile.rfind('_');
                if (pos != std::string::npos) labelFile = labelFile.substr(0, pos) + "_label";
                std::string lab = readSysfsValue(hwmonPath / labelFile);
                if (!lab.empty()) label = lab;
                // Compose a reasonably stable id
                std::string id = idPrefix + "_" + chipName + "_" + label;
                // normalize id: replace spaces / slashes
                std::replace(id.begin(), id.end(), ' ', '_');
                std::replace(id.begin(), id.end(), '/', '_');
                // Map attrPath -> "name" field, label -> label
                SensorData sd = { id, type, attrPath, label, v, units, st, hwmonPath.string(), std::chrono::system_clock::now() };
                addSensorIfNew(sensors, sd, seen);
            }
            catch (...) {
                return;
            }
        };

    // temperatures (millideg to degC)
    for (int i = 1; i <= 12; ++i) {
        pushFromAttr("temp" + std::to_string(i) + "_input", "thermal", "thermal", "Temp" + std::to_string(i), "C",
            [](long long vv) { return vv / 1000.0; },
            [](double v) { return v > 85.0 ? SensorStatus::CRITICAL : SensorStatus::OK; });
    }
    // voltages (in0_input ... millivolt -> volts)
    for (int i = 0; i < 12; ++i) {
        pushFromAttr("in" + std::to_string(i) + "_input", "voltage", "voltage", "V" + std::to_string(i), "V",
            [](long long vv) { return vv / 1000.0; }, nullptr);
    }
    // fans
    for (int i = 1; i <= 8; ++i) {
        pushFromAttr("fan" + std::to_string(i) + "_input", "fan", "fan", "Fan" + std::to_string(i), "RPM",
            nullptr, [](double v) { return v < 100 ? SensorStatus::WARN : SensorStatus::OK; });
    }
    // pwm controls
    for (int i = 1; i <= 8; ++i) {
        pushFromAttr("pwm" + std::to_string(i), "pwm", "pwm", "PWM" + std::to_string(i), "0-255",
            nullptr, nullptr);
    }
    // power / energy / current
    for (int i = 1; i <= 4; ++i) {
        pushFromAttr("power" + std::to_string(i) + "_average", "power", "power", "Power" + std::to_string(i), "W",
            [](long long vv) { return vv / 1000000.0; }, nullptr);
        pushFromAttr("energy" + std::to_string(i) + "_input", "energy", "energy", "Energy" + std::to_string(i), "J",
            [](long long vv) { return vv / 1000000.0; }, nullptr);
        pushFromAttr("curr" + std::to_string(i) + "_input", "current", "current", "Current" + std::to_string(i), "A",
            [](long long vv) { return vv / 1000.0; }, nullptr);
    }
}

// Map hwmon chip name -> path
static std::map<std::string, std::string> discoverHwmonByName() {
    std::map<std::string, std::string> m;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/hwmon")) {
        std::string name = readSysfsValue(e.path() / "name");
        if (name.empty()) name = e.path().filename().string();
        m[name] = e.path().string();
    }
    return m;
}
#endif // __linux__

// ---------------------
// Windows helpers: scan LibreHardwareMonitor and generic CIM sensor queries (must be called while COM and loc/svc are available)
// ---------------------
#ifdef _WIN32
static void scanLibreHardwareMonitor(IWbemLocator* pLoc, std::vector<SensorData>& sensors, std::unordered_set<std::string>& seen) {
    IWbemServices* pSvcL = nullptr;
    if (FAILED(pLoc->ConnectServer(BSTR(L"ROOT\\LibreHardwareMonitor"), NULL, NULL, 0, NULL, 0, 0, &pSvcL))) {
        return;
    }
    IEnumWbemClassObject* pEnum = nullptr;
    if (SUCCEEDED(pSvcL->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT Identifier, Name, Value, SensorType FROM Sensor"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum))) {
        IWbemClassObject* pObj = nullptr; ULONG ret;
        while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK) {
            VARIANT vtId; VARIANT vtName; VARIANT vtValue; VARIANT vtType;
            VariantInit(&vtId); VariantInit(&vtName); VariantInit(&vtValue); VariantInit(&vtType);
            pObj->Get(L"Identifier", 0, &vtId, NULL, NULL);
            pObj->Get(L"Name", 0, &vtName, NULL, NULL);
            pObj->Get(L"Value", 0, &vtValue, NULL, NULL);
            pObj->Get(L"SensorType", 0, &vtType, NULL, NULL);
            if ((vtId.vt == VT_BSTR) && (vtValue.vt == VT_R4 || vtValue.vt == VT_R8)) {
                std::string id = bstr_to_str(vtId.bstrVal);
                std::string name = (vtName.vt == VT_BSTR) ? bstr_to_str(vtName.bstrVal) : id;
                double val = (vtValue.vt == VT_R4) ? vtValue.fltVal : vtValue.dblVal;
                std::string t = (vtType.vt == VT_BSTR) ? bstr_to_str(vtType.bstrVal) : "sensor";
                std::string sid = "librehwmon_" + id;
                SensorData sd = { sid, t, name, name, val, "", SensorStatus::OK, "LibreHardwareMonitor", std::chrono::system_clock::now() };
                addSensorIfNew(sensors, sd, seen);
            }
            VariantClear(&vtId); VariantClear(&vtName); VariantClear(&vtValue); VariantClear(&vtType);
            pObj->Release();
        }
        pEnum->Release();
    }
    pSvcL->Release();
}

static void scanGenericCimSensor(IWbemServices* pSvc, std::vector<SensorData>& sensors, std::unordered_set<std::string>& seen, const wchar_t* query) {
    IEnumWbemClassObject* pEnum = nullptr;
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(query), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum))) {
        IWbemClassObject* pObj = nullptr; ULONG ret;
        while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK) {
            VARIANT vtName; VARIANT vtProp;
            VariantInit(&vtName); VariantInit(&vtProp);
            if (SUCCEEDED(pObj->Get(L"Name", 0, &vtName, nullptr, nullptr))) {
                // try CurrentReading, LoadPercentage, Value, etc.
                HRESULT got = pObj->Get(L"CurrentReading", 0, &vtProp, nullptr, nullptr);
                if (FAILED(got)) got = pObj->Get(L"LoadPercentage", 0, &vtProp, nullptr, nullptr);
                if (FAILED(got)) got = pObj->Get(L"Value", 0, &vtProp, nullptr, nullptr);
                if (SUCCEEDED(got)) {
                    double value = 0.0;
                    if (vtProp.vt == VT_UI1) value = vtProp.bVal;
                    else if (vtProp.vt == VT_UI2 || vtProp.vt == VT_UINT) value = vtProp.uintVal;
                    else if (vtProp.vt == VT_I4) value = vtProp.lVal;
                    else if (vtProp.vt == VT_R4) value = vtProp.fltVal;
                    else if (vtProp.vt == VT_R8) value = vtProp.dblVal;
                    std::string name = bstr_to_str(vtName.bstrVal);
                    std::string id = "cim_" + name;
                    SensorData sd = { id, "cim", name, name, value, "", SensorStatus::OK, "WMI", std::chrono::system_clock::now() };
                    addSensorIfNew(sensors, sd, seen);
                }
            }
            VariantClear(&vtName); VariantClear(&vtProp);
            pObj->Release();
        }
        pEnum->Release();
    }
}
#endif // _WIN32

// Optional GPU vendor hooks (compile with HAS_NVML and link nvml if available)
#ifdef HAS_NVML
#include <nvml.h>
static void scanNvml(std::vector<SensorData>& sensors, std::unordered_set<std::string>& seen) {
    nvmlReturn_t rc = nvmlInit();
    if (rc != NVML_SUCCESS) return;
    unsigned int deviceCount = 0;
    if (nvmlDeviceGetCount(&deviceCount) != NVML_SUCCESS) {
        nvmlShutdown();
        return;
    }
    for (unsigned int i = 0; i < deviceCount; ++i) {
        nvmlDevice_t dev;
        if (nvmlDeviceGetHandleByIndex(i, &dev) != NVML_SUCCESS) continue;
        char name[64] = { 0 };
        nvmlDeviceGetName(dev, name, sizeof(name));
        unsigned int temp = 0;
        if (nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) {
            std::string id = std::string("nvml_gpu") + std::to_string(i) + "_temp";
            SensorData sd = { id, "gpu", "temperature", name, (double)temp, "C", SensorStatus::OK, "NVML", std::chrono::system_clock::now() };
            addSensorIfNew(sensors, sd, seen);
        }
        unsigned int power = 0;
        if (nvmlDeviceGetPowerUsage(dev, &power) == NVML_SUCCESS) {
            // NVML returns milliwatts
            SensorData sd = { std::string("nvml_gpu") + std::to_string(i) + "_power", "power", "power", name, (double)power / 1000.0, "W", SensorStatus::OK, "NVML", std::chrono::system_clock::now() };
            addSensorIfNew(sensors, sd, seen);
        }
    }
    nvmlShutdown();
}
#endif // HAS_NVML

// ---------------------
// Main exported function: listSensors()
// We keep your original logic intact and append additive scans safely.
// ---------------------

std::vector<SensorData> SensorManager::listSensors() {
    std::vector<SensorData> sensors;

#ifdef _WIN32
    // =================================================
    // Windows WMI Implementation
    // =================================================
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        std::cerr << "Failed to initialize COM library. Error code = 0x" << std::hex << hr << std::endl;
        return sensors;
    }

    IWbemLocator* pLoc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hr)) {
        CoUninitialize();
        return sensors;
    }

    IWbemServices* pSvc = nullptr;
    hr = pLoc->ConnectServer(BSTR(L"ROOT\\CIMV2"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    if (FAILED(hr)) {
        pLoc->Release();
        CoUninitialize();
        return sensors;
    }

    IEnumWbemClassObject* pEnumerator = nullptr;

    // --- 1. CPU Temperature (from ROOT\WMI) ---
    IWbemServices* pSvcWmi = nullptr;
    if (SUCCEEDED(pLoc->ConnectServer(BSTR(L"ROOT\\WMI"), NULL, NULL, 0, NULL, 0, 0, &pSvcWmi))) {
        if (SUCCEEDED(pSvcWmi->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT * FROM MSAcpi_ThermalZoneTemperature"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
            IWbemClassObject* pClassObj = nullptr;
            ULONG uReturn = 0;
            if (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
                VARIANT vtProp;
                VariantInit(&vtProp);
                if (SUCCEEDED(pClassObj->Get(L"CurrentTemperature", 0, &vtProp, nullptr, nullptr))) {
                    if (vtProp.vt == VT_UI4 || vtProp.vt == VT_UINT) {
                        double tempC = ((double)vtProp.uintVal / 10.0) - 273.15;
                        sensors.push_back({ "thermal_cpu", "thermal", "CurrentTemperature", "CPU Temp", tempC, "C", tempC > 85.0 ? SensorStatus::CRITICAL : SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                    }
                    VariantClear(&vtProp);
                }
                pClassObj->Release();
            }
            pEnumerator->Release();
        }
        pSvcWmi->Release();
    }

    // --- 2. CPU Load ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT LoadPercentage FROM Win32_Processor"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtProp;
            VariantInit(&vtProp);
            if (SUCCEEDED(pClassObj->Get(L"LoadPercentage", 0, &vtProp, nullptr, nullptr))) {
                if (vtProp.vt == VT_UI2 || vtProp.vt == VT_UINT) {
                    sensors.push_back({ "cpu_load", "cpu", "LoadPercentage", "CPU Load", (double)vtProp.uintVal, "%", (double)vtProp.uintVal > 90.0 ? SensorStatus::WARN : SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                }
                VariantClear(&vtProp);
            }
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --- 3. Memory Usage ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT TotalVisibleMemorySize, FreePhysicalMemory FROM Win32_OperatingSystem"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtTotal, vtFree;
            VariantInit(&vtTotal);
            VariantInit(&vtFree);
            if (SUCCEEDED(pClassObj->Get(L"TotalVisibleMemorySize", 0, &vtTotal, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"FreePhysicalMemory", 0, &vtFree, nullptr, nullptr))) {
                if (vtTotal.vt == VT_BSTR && vtFree.vt == VT_BSTR) {
                    unsigned long long totalMem = std::stoull(bstr_to_str(vtTotal.bstrVal)) * 1024;
                    unsigned long long freeMem = std::stoull(bstr_to_str(vtFree.bstrVal)) * 1024;
                    if (totalMem > 0) {
                        double usedMemPercent = 100.0 - ((double)freeMem / totalMem * 100.0);
                        sensors.push_back({ "mem_usage", "memory", "UsedMemory", "Memory Usage", usedMemPercent, "%", usedMemPercent > 85.0 ? SensorStatus::WARN : SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                    }
                }
            }
            VariantClear(&vtTotal);
            VariantClear(&vtFree);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --- 4. Disk Space ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT FreeSpace, Size, Name FROM Win32_LogicalDisk WHERE DriveType=3"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        while (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtFree, vtSize, vtName;
            VariantInit(&vtFree); VariantInit(&vtSize); VariantInit(&vtName);
            if (SUCCEEDED(pClassObj->Get(L"FreeSpace", 0, &vtFree, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"Size", 0, &vtSize, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"Name", 0, &vtName, nullptr, nullptr))) {
                if (vtFree.vt == VT_BSTR && vtSize.vt == VT_BSTR && vtName.vt == VT_BSTR) {
                    unsigned long long freeSpace = std::stoull(bstr_to_str(vtFree.bstrVal));
                    unsigned long long totalSize = std::stoull(bstr_to_str(vtSize.bstrVal));
                    double freePercent = (totalSize > 0) ? (double)freeSpace / totalSize * 100.0 : 0.0;
                    std::string diskName = bstr_to_str(vtName.bstrVal);
                    sensors.push_back({ "disk_free_" + diskName, "disk", "FreeSpace", "Disk " + diskName + " Free", freePercent, "%", freePercent < 15.0 ? SensorStatus::WARN : SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                }
            }
            VariantClear(&vtFree); VariantClear(&vtSize); VariantClear(&vtName);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --- 5. Fan Speed ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT * FROM Win32_Fan"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        int fanIndex = 1;
        while (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtSpeed;
            VariantInit(&vtSpeed);
            if (SUCCEEDED(pClassObj->Get(L"DesiredSpeed", 0, &vtSpeed, nullptr, nullptr))) {
                if (vtSpeed.vt == VT_I8 || vtSpeed.vt == VT_UI8) {
                    sensors.push_back({ "fan_" + std::to_string(fanIndex), "fan", "DesiredSpeed", "Fan " + std::to_string(fanIndex), (double)vtSpeed.llVal, "RPM", SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                }
                fanIndex++;
            }
            VariantClear(&vtSpeed);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --- 6. Voltage and Current Probes ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT * FROM Win32_VoltageProbe"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        while (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtValue, vtName;
            VariantInit(&vtValue); VariantInit(&vtName);
            if (SUCCEEDED(pClassObj->Get(L"CurrentReading", 0, &vtValue, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"Name", 0, &vtName, nullptr, nullptr))) {
                if (vtValue.vt == VT_I4 && vtName.vt == VT_BSTR) {
                    sensors.push_back({ "voltage_" + bstr_to_str(vtName.bstrVal), "voltage", "CurrentReading", bstr_to_str(vtName.bstrVal), (double)vtValue.lVal / 1000.0, "V", SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                }
            }
            VariantClear(&vtValue); VariantClear(&vtName);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT * FROM Win32_CurrentProbe"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        while (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtValue, vtName;
            VariantInit(&vtValue); VariantInit(&vtName);
            if (SUCCEEDED(pClassObj->Get(L"CurrentReading", 0, &vtValue, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"Name", 0, &vtName, nullptr, nullptr))) {
                if (vtValue.vt == VT_I4 && vtName.vt == VT_BSTR) {
                    sensors.push_back({ "current_" + bstr_to_str(vtName.bstrVal), "current", "CurrentReading", bstr_to_str(vtName.bstrVal), (double)vtValue.lVal / 1000.0, "A", SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                }
            }
            VariantClear(&vtValue); VariantClear(&vtName);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --- 7. Battery Status ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT EstimatedChargeRemaining, BatteryStatus FROM Win32_Battery"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtCharge, vtStatus;
            VariantInit(&vtCharge); VariantInit(&vtStatus);
            if (SUCCEEDED(pClassObj->Get(L"EstimatedChargeRemaining", 0, &vtCharge, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"BatteryStatus", 0, &vtStatus, nullptr, nullptr))) {
                if (vtCharge.vt == VT_UI1) {
                    sensors.push_back({ "battery_charge", "power", "Charge", "Battery Charge", (double)vtCharge.bVal, "%", (double)vtCharge.bVal < 20.0 ? SensorStatus::WARN : SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                }
            }
            VariantClear(&vtCharge); VariantClear(&vtStatus);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --- 8. System Context (Enclosure/Chassis) ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT ChassisTypes FROM Win32_SystemEnclosure"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtChassis;
            VariantInit(&vtChassis);
            if (SUCCEEDED(pClassObj->Get(L"ChassisTypes", 0, &vtChassis, nullptr, nullptr))) {
                if (vtChassis.vt & VT_ARRAY) {
                    // This is a more complex property to parse, but it provides valuable context
                }
            }
            VariantClear(&vtChassis);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --- 9. Network I/O ---
    if (SUCCEEDED(pSvc->ExecQuery(BSTR(L"WQL"), BSTR(L"SELECT Name, BytesSentPerSec, BytesReceivedPerSec FROM Win32_PerfRawData_Tcpip_NetworkInterface"), WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator))) {
        IWbemClassObject* pClassObj = nullptr;
        ULONG uReturn = 0;
        while (pEnumerator->Next(WBEM_INFINITE, 1, &pClassObj, &uReturn) == S_OK) {
            VARIANT vtName, vtSent, vtRecv;
            VariantInit(&vtName); VariantInit(&vtSent); VariantInit(&vtRecv);
            if (SUCCEEDED(pClassObj->Get(L"Name", 0, &vtName, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"BytesSentPerSec", 0, &vtSent, nullptr, nullptr)) && SUCCEEDED(pClassObj->Get(L"BytesReceivedPerSec", 0, &vtRecv, nullptr, nullptr))) {
                if (vtName.vt == VT_BSTR && vtSent.vt == VT_BSTR && vtRecv.vt == VT_BSTR) {
                    std::string ifaceName = bstr_to_str(vtName.bstrVal);
                    std::replace(ifaceName.begin(), ifaceName.end(), ' ', '_');
                    std::replace(ifaceName.begin(), ifaceName.end(), '(', '_');
                    std::replace(ifaceName.begin(), ifaceName.end(), ')', '_');
                    unsigned long long sent = std::stoull(bstr_to_str(vtSent.bstrVal));
                    unsigned long long received = std::stoull(bstr_to_str(vtRecv.bstrVal));
                    sensors.push_back({ "net_sent_" + ifaceName, "net", "BytesSent", ifaceName + " Sent", (double)sent, "B/s", SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                    sensors.push_back({ "net_recv_" + ifaceName, "net", "BytesReceived", ifaceName + " Received", (double)received, "B/s", SensorStatus::OK, "WMI", std::chrono::system_clock::now() });
                }
            }
            VariantClear(&vtName); VariantClear(&vtSent); VariantClear(&vtRecv);
            pClassObj->Release();
        }
        pEnumerator->Release();
    }

    // --------------------------
    // ADDITIVE: After core WMI scans, run additional aggregators (LibreHardwareMonitor and generic CIM numeric sensors)
    // These are additive and will not remove or alter existing entries. They use a seen set to avoid duplicates.
    // --------------------------
    {
        std::unordered_set<std::string> seen;
        // populate seen with existing sensor ids (best-effort)
        for (const auto& sd : sensors) {
            if (!sd.id.empty()) seen.insert(sd.id);
            else seen.insert(sd.type + "_" + sd.name);
        }

        // scan LibreHardwareMonitor namespace if present
        // note: scanLibreHardwareMonitor requires pLoc to still be valid
        scanLibreHardwareMonitor(pLoc, sensors, seen);

        // scan generic CIM/Numeric sensors to capture additional sources
        scanGenericCimSensor(pSvc, sensors, seen, L"SELECT Name, CurrentReading FROM CIM_NumericSensor");
        scanGenericCimSensor(pSvc, sensors, seen, L"SELECT Name, CurrentReading FROM Win32_TemperatureProbe");
        scanGenericCimSensor(pSvc, sensors, seen, L"SELECT Name, LoadPercentage FROM Win32_Processor");
    }

    pSvc->Release();
    pLoc->Release();
    CoUninitialize();

#elif __linux__
    // =================================================
    // Linux /sys/class and /proc Implementation
    // =================================================
    // --- 1. Comprehensive HWMON Scan for Temp, Fan, Voltage, Power, Current, Energy ---
    for (const auto& hwmon_entry : std::filesystem::directory_iterator("/sys/class/hwmon")) {
        if (!hwmon_entry.is_directory()) continue;

        std::string deviceName;
        std::ifstream nameFile(hwmon_entry.path() / "name");
        if (nameFile.is_open()) {
            std::getline(nameFile, deviceName);
        }

        for (const auto& sensor_entry : std::filesystem::directory_iterator(hwmon_entry.path())) {
            std::string filename = sensor_entry.path().filename().string();
            std::string type;
            std::string item;
            int number = 0;

            if (filename.rfind("temp", 0) == 0) type = "temp";
            else if (filename.rfind("fan", 0) == 0) type = "fan";
            else if (filename.rfind("in", 0) == 0) type = "in";
            else if (filename.rfind("power", 0) == 0) type = "power";
            else if (filename.rfind("curr", 0) == 0) type = "curr";
            else if (filename.rfind("energy", 0) == 0) type = "energy";
            else continue;

            size_t underscore_pos = filename.find('_');
            if (underscore_pos == std::string::npos) continue;

            try {
                number = std::stoi(filename.substr(type.length(), underscore_pos - type.length()));
                item = filename.substr(underscore_pos + 1);
            }
            catch (...) {
                continue;
            }

            if (item == "input" || item == "average") {
                std::ifstream valueFile(sensor_entry.path());
                if (!valueFile.is_open()) continue;
                double value;
                valueFile >> value;

                std::string label = deviceName + " " + type + std::to_string(number);
                std::ifstream labelFile(hwmon_entry.path() / (type + std::to_string(number) + "_label"));
                if (labelFile.is_open()) {
                    std::string tempLabel;
                    std::getline(labelFile, tempLabel);
                    if (!tempLabel.empty()) label = tempLabel;
                }

                if (type == "temp") {
                    sensors.push_back({ "thermal_" + label, "thermal", filename, label, value / 1000.0, "C", (value / 1000.0) > 85.0 ? SensorStatus::CRITICAL : SensorStatus::OK, sensor_entry.path().string(), std::chrono::system_clock::now() });
                }
                else if (type == "fan") {
                    sensors.push_back({ "fan_" + label, "fan", filename, label, value, "RPM", value < 100 ? SensorStatus::WARN : SensorStatus::OK, sensor_entry.path().string(), std::chrono::system_clock::now() });
                }
                else if (type == "in") {
                    sensors.push_back({ "voltage_" + label, "voltage", filename, label, value / 1000.0, "V", SensorStatus::OK, sensor_entry.path().string(), std::chrono::system_clock::now() });
                }
                else if (type == "power") {
                    sensors.push_back({ "power_" + label, "power", filename, label, value / 1000000.0, "W", SensorStatus::OK, sensor_entry.path().string(), std::chrono::system_clock::now() });
                }
                else if (type == "curr") {
                    sensors.push_back({ "current_" + label, "current", filename, label, value / 1000.0, "A", SensorStatus::OK, sensor_entry.path().string(), std::chrono::system_clock::now() });
                }
                else if (type == "energy") {
                    sensors.push_back({ "energy_" + label, "energy", filename, label, value / 1000000.0, "J", SensorStatus::OK, sensor_entry.path().string(), std::chrono::system_clock::now() });
                }
            }
        }
    }

    // --- 2. Battery ---
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply")) {
        if (entry.is_directory() && entry.path().filename().string().find("BAT") != std::string::npos) {
            std::ifstream capFile(entry.path() / "capacity");
            if (capFile.is_open()) {
                double capacity;
                capFile >> capacity;
                sensors.push_back({ "battery_charge", "power", entry.path().filename().string(), "Battery", capacity, "%", capacity < 20.0 ? SensorStatus::WARN : SensorStatus::OK, entry.path().string(), std::chrono::system_clock::now() });
            }
        }
    }

    // --- 3. CPU Load ---
    CpuTimes t1 = getCpuTimes();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CpuTimes t2 = getCpuTimes();
    double totalDelta = t2.getTotal() - t1.getTotal();
    double idleDelta = t2.getIdle() - t1.getIdle();
    double cpuUsage = (totalDelta > 0) ? (1.0 - idleDelta / totalDelta) * 100.0 : 0.0;
    sensors.push_back({ "cpu_load", "cpu", "Usage", "CPU Load", cpuUsage, "%", cpuUsage > 90.0 ? SensorStatus::WARN : SensorStatus::OK, "/proc/stat", std::chrono::system_clock::now() });

    // --- 4. Memory Usage ---
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    long long memTotal = 0, memAvailable = 0;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            // parse number after label
            std::istringstream iss(line.substr(9));
            iss >> memTotal;
        }
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream iss(line.substr(13));
            iss >> memAvailable;
        }
    }
    if (memTotal > 0) {
        double memUsedPercent = 100.0 - ((double)memAvailable / memTotal * 100.0);
        sensors.push_back({ "mem_usage", "memory", "UsedMemory", "Memory Usage", memUsedPercent, "%", memUsedPercent > 85.0 ? SensorStatus::WARN : SensorStatus::OK, "/proc/meminfo", std::chrono::system_clock::now() });
    }

    // --- 5. Disk Space ---
    struct statvfs stat;
    if (statvfs("/", &stat) == 0) {
        unsigned long long totalSpace = stat.f_blocks * stat.f_frsize;
        unsigned long long freeSpace = stat.f_bavail * stat.f_frsize;
        if (totalSpace > 0) {
            double freePercent = (double)freeSpace / totalSpace * 100.0;
            sensors.push_back({ "disk_free_root", "disk", "FreeSpace", "Disk / Free", freePercent, "%", freePercent < 15.0 ? SensorStatus::WARN : SensorStatus::OK, "statvfs", std::chrono::system_clock::now() });
        }
    }

    // --- 6. Network I/O ---
    std::ifstream net_dev("/proc/net/dev");
    std::string net_line;
    std::getline(net_dev, net_line); // Skip header
    std::getline(net_dev, net_line); // Skip header
    while (std::getline(net_dev, net_line)) {
        std::stringstream ss(net_line);
        std::string iface_name;
        long long recv_bytes, recv_packets, recv_errs, recv_drop;
        long long trans_bytes, trans_packets, trans_errs, trans_drop;
        ss >> iface_name;
        if (iface_name.back() == ':') {
            iface_name.pop_back();
        }
        ss >> recv_bytes >> recv_packets >> recv_errs >> recv_drop;
        ss >> trans_bytes >> trans_packets >> trans_errs >> trans_drop;
        sensors.push_back({ "net_recv_" + iface_name, "net", "ReceivedBytes", iface_name + " Received", (double)recv_bytes, "Bytes", SensorStatus::OK, "/proc/net/dev", std::chrono::system_clock::now() });
        sensors.push_back({ "net_sent_" + iface_name, "net", "SentBytes", iface_name + " Sent", (double)trans_bytes, "Bytes", SensorStatus::OK, "/proc/net/dev", std::chrono::system_clock::now() });
    }

    // --------------------------
    // ADDITIVE: After core sysfs and /proc scans, run a second pass hwmon parser and optional GPU hooks.
    // This pass uses the unified hwmon parser to capture labels, thresholds, PWM, energy, etc., and avoids duplicates.
    // --------------------------
    {
        std::unordered_set<std::string> seen;
        for (const auto& sd : sensors) {
            if (!sd.id.empty()) seen.insert(sd.id);
            else seen.insert(sd.type + "_" + sd.name);
        }

        // run exhaustive hwmon parsing for each hwmon device
        for (const auto& hw : std::filesystem::directory_iterator("/sys/class/hwmon")) {
            if (!hw.is_directory()) continue;
            parseHwmonDevice(hw.path(), sensors, seen);
        }

        // Optional NVML (if compiled with HAS_NVML)
#ifdef HAS_NVML
        scanNvml(sensors, seen);
#endif
    }

#else
    std::cerr << "SensorManager: Unsupported platform." << std::endl;
#endif

    return sensors;
}

Copyright © 2025 Cadell Richard Anderson

// =================================================================
// SensorManager.h
// Defines the sensor data structures and manager class.
// =================================================================
#pragma once
#include <string>
#include <vector>
#include <chrono>

enum class SensorStatus {
    OK,
    WARN,
    CRITICAL,
    UNKNOWN
};

struct SensorData {
    std::string id;
    std::string type;
    std::string name;
    std::string label;
    double value;
    std::string unit;
    SensorStatus status;
    std::string source;
    std::chrono::system_clock::time_point timestamp;
};

class SensorManager {
public:
    // Returns a list of discovered sensors (platform-specific)
    static std::vector<SensorData> listSensors();
};

#pragma once
#include "OmniConfig.h"
#include "OmniAIManager.h"   // for SamplingProfile definition
#include "SensorManager.h"   // for SensorData struct definition
#include <thread>
#include <atomic>
#include <string>
#include <vector>

class DaemonMonitor {
public:
    DaemonMonitor();
    ~DaemonMonitor();

    void start(const ConfigState& config);
    void stop();
    std::string getStatus() const;

    // Expose to CLI
    void captureTileTelemetry();

    // Moved from private: so it’s callable externally
    SamplingProfile decideProfile(
        const std::vector<SensorData>& sensors,
        const ConfigState& currentConfig
    );

private:
    void monitorLoop(ConfigState config);

    std::atomic<bool> isRunning;
    std::thread monitorThread;
};

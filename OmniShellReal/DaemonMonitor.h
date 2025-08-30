Copyright © 2025 Cadell Richard Anderson

#pragma once
#include "OmniConfig.h"
#include "OmniAIManager.h"   
#include "SensorManager.h"  
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


    SamplingProfile decideProfile(
        const std::vector<SensorData>& sensors,
        const ConfigState& currentConfig
    );

private:
    void monitorLoop(ConfigState config);

    std::atomic<bool> isRunning;
    std::thread monitorThread;
};

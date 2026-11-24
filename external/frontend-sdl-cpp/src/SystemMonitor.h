#pragma once

#include <cstdint>

struct SystemStats
{
    float cpuPct{0.0f};
    long memUsedMB{0};
    long memTotalMB{0};
    float tempC{0.0f};
    bool hasTemp{false};
};

class SystemMonitor
{
public:
    SystemStats Sample();

private:
    uint64_t _prevCpuTotal{0};
    uint64_t _prevCpuIdle{0};
};

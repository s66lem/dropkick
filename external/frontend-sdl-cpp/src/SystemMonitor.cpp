#include "SystemMonitor.h"

#include <algorithm>

#if defined(__linux__)
#include <fstream>
#include <limits>
#include <string>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
uint64_t FileTimeTicks(const FILETIME& time)
{
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
}
#endif

SystemStats SystemMonitor::Sample()
{
    SystemStats stats;
    uint64_t idle = 0;
    uint64_t total = 0;

#if defined(__linux__)
    std::ifstream cpu("/proc/stat");
    std::string label;
    uint64_t user, nice, system, idleTicks, io, irq, softIrq, steal;
    if (cpu >> label >> user >> nice >> system >> idleTicks >> io >> irq >> softIrq >> steal)
    {
        idle = idleTicks + io;
        total = user + nice + system + idle + irq + softIrq + steal;
    }

    std::ifstream memory("/proc/meminfo");
    std::string key;
    long value, memTotal = 0, memAvailable = -1, fallbackAvailable = 0;
    while (memory >> key >> value)
    {
        if (key == "MemTotal:") { memTotal = value; }
        if (key == "MemAvailable:") { memAvailable = value; break; }
        if (key == "MemFree:" || key == "Buffers:" || key == "Cached:") { fallbackAvailable += value; }
        memory.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    if (memAvailable < 0) { memAvailable = fallbackAvailable; }
    stats.memTotalMB = memTotal / 1024;
    stats.memUsedMB = std::max(0L, memTotal - memAvailable) / 1024;

    std::ifstream thermal("/sys/class/thermal/thermal_zone0/temp");
    long milliDegrees;
    if (thermal >> milliDegrees)
    {
        stats.tempC = static_cast<float>(milliDegrees) / 1000.0f;
        stats.hasTemp = true;
    }
#elif defined(_WIN32)
    FILETIME idleTime, kernelTime, userTime;
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime))
    {
        idle = FileTimeTicks(idleTime);
        total = FileTimeTicks(kernelTime) + FileTimeTicks(userTime);
    }

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory))
    {
        stats.memTotalMB = static_cast<long>(memory.ullTotalPhys / (1024 * 1024));
        stats.memUsedMB = static_cast<long>((memory.ullTotalPhys - memory.ullAvailPhys) / (1024 * 1024));
    }
#endif

    if (total > 0)
    {
        if (_prevCpuTotal > 0 && total > _prevCpuTotal && idle >= _prevCpuIdle)
        {
            const double idleFraction = static_cast<double>(idle - _prevCpuIdle) / static_cast<double>(total - _prevCpuTotal);
            stats.cpuPct = static_cast<float>(std::clamp((1.0 - idleFraction) * 100.0, 0.0, 100.0));
        }
        _prevCpuTotal = total;
        _prevCpuIdle = idle;
    }
    return stats;
}

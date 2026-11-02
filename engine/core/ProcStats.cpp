#include "ProcStats.h"

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN 由 CMake 全局加上；psapi.h 需要 windows.h 先。
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#include <cstdio>
#endif

namespace core {

ProcMemory queryProcessMemory() {
    ProcMemory m;

#if defined(_WIN32)
    // PROCESS_MEMORY_COUNTERS_EX 比基础版多 PrivateUsage 一个字段，
    // 所以 cb 必须给 sizeof(EX)，否则系统按基础版截断、PrivateUsage 是 0。
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = static_cast<DWORD>(sizeof(pmc));
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        constexpr double kMB = 1024.0 * 1024.0;
        m.workingSetMB = static_cast<double>(pmc.WorkingSetSize) / kMB;
        m.peakWorkingSetMB = static_cast<double>(pmc.PeakWorkingSetSize) / kMB;
        m.privateMB = static_cast<double>(pmc.PrivateUsage) / kMB;
        m.valid = true;
    }
#elif defined(__linux__)
    // /proc/self/statm: size resident shared text lib data dt（单位=页）。
    // 峰值这里拿不到（要读 status 的 VmHWM），一并读一下 —— 两行都失败
    // 就保持 valid=false。
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        long resident = 0;
        if (std::fscanf(f, "%*d %ld", &resident) == 1 && resident > 0) {
            const long page = sysconf(_SC_PAGESIZE);
            constexpr double kMB = 1024.0 * 1024.0;
            m.workingSetMB =
                static_cast<double>(resident) * static_cast<double>(page) / kMB;
            m.privateMB = m.workingSetMB;  // 没有可靠来源，不假装有
            m.valid = true;
        }
        std::fclose(f);
    }
    if (FILE* g = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), g)) {
            long kb = 0;
            if (std::sscanf(line, "VmHWM: %ld kB", &kb) == 1 && kb > 0) {
                m.peakWorkingSetMB = static_cast<double>(kb) / 1024.0;
            }
        }
        std::fclose(g);
    }
#endif

    return m;
}

} // namespace core

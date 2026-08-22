#include "windows_process_memory_probe.h"

// Windows SDK contract상 psapi.h보다 windows.h가 먼저 와야 한다.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on

namespace flexraw::platform
{

// 목적: Windows current process working set을 Platform snapshot으로 변환
// 입력: 없음
// 출력: GetProcessMemoryInfo 성공 시 resident/peak resident byte snapshot
std::optional<ProcessMemorySnapshot> WindowsProcessMemoryProbe::snapshot() const
{
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0)
    {
        return std::nullopt;
    }

    const auto residentBytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
    const auto peakResidentBytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    if (residentBytes == 0 || peakResidentBytes < residentBytes)
    {
        return std::nullopt;
    }
    return ProcessMemorySnapshot{residentBytes, peakResidentBytes};
}

}  // namespace flexraw::platform

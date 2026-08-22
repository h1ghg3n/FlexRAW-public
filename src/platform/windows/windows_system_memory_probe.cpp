#include "windows_system_memory_probe.h"

#include <windows.h>

namespace flexraw::platform
{

// 목적: Windows physical memory 상태를 Platform snapshot으로 변환
// 입력: 없음
// 출력: GlobalMemoryStatusEx 성공 시 total/available byte snapshot
std::optional<SystemMemorySnapshot> WindowsSystemMemoryProbe::snapshot() const
{
    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus) == 0)
    {
        return std::nullopt;
    }
    return SystemMemorySnapshot{memoryStatus.ullTotalPhys, memoryStatus.ullAvailPhys};
}

}  // namespace flexraw::platform

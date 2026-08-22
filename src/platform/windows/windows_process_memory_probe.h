#pragma once

#include "process_memory_probe.h"

namespace flexraw::platform
{

class WindowsProcessMemoryProbe final : public IProcessMemoryProbe
{
public:
    // 목적: Windows current process working set을 Platform snapshot으로 변환
    // 입력: 없음
    // 출력: GetProcessMemoryInfo 성공 시 resident/peak resident byte snapshot
    [[nodiscard]] std::optional<ProcessMemorySnapshot> snapshot() const override;
};

}  // namespace flexraw::platform

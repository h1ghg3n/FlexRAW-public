#pragma once

#include "process_memory_probe.h"

namespace flexraw::platform
{

class LinuxProcessMemoryProbe final : public IProcessMemoryProbe
{
public:
    // 목적: Linux current process의 /proc/self/status memory 값을 Platform snapshot으로 변환
    // 입력: 없음
    // 출력: VmRSS/VmHWM 조회 성공 시 resident/peak resident byte snapshot
    [[nodiscard]] std::optional<ProcessMemorySnapshot> snapshot() const override;
};

}  // namespace flexraw::platform

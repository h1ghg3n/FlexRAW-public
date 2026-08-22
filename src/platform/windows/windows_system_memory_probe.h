#pragma once

#include "system_memory_probe.h"

namespace flexraw::platform
{

class WindowsSystemMemoryProbe final : public ISystemMemoryProbe
{
public:
    // 목적: Windows physical memory 상태를 Platform snapshot으로 변환
    // 입력: 없음
    // 출력: GlobalMemoryStatusEx 성공 시 total/available byte snapshot
    [[nodiscard]] std::optional<SystemMemorySnapshot> snapshot() const override;
};

}  // namespace flexraw::platform

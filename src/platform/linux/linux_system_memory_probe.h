#pragma once

#include "system_memory_probe.h"

namespace flexraw::platform
{

class LinuxSystemMemoryProbe final : public ISystemMemoryProbe
{
public:
    // 목적: /proc/meminfo의 MemTotal/MemAvailable을 Platform snapshot으로 변환
    // 입력: 없음
    // 출력: 두 값이 유효하면 byte 단위 snapshot, 읽기 실패면 nullopt
    [[nodiscard]] std::optional<SystemMemorySnapshot> snapshot() const override;
};

}  // namespace flexraw::platform

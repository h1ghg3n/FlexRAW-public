#pragma once

#include <memory>

#include "system_memory_probe.h"

namespace flexraw::platform
{

// 목적: 현재 CMake target OS에 맞는 system memory probe 생성
// 입력: 없음
// 출력: application bootstrap이 소유할 concrete OS Adapter
[[nodiscard]] std::unique_ptr<ISystemMemoryProbe> createCurrentSystemMemoryProbe();

}  // namespace flexraw::platform

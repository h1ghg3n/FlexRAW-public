#pragma once

#include <memory>

#include "process_memory_probe.h"

namespace flexraw::platform
{

// 목적: 현재 CMake target OS에 맞는 process memory probe 생성
// 입력: 없음
// 출력: application 또는 benchmark bootstrap이 소유할 concrete OS Adapter
[[nodiscard]] std::unique_ptr<IProcessMemoryProbe> createCurrentProcessMemoryProbe();

}  // namespace flexraw::platform

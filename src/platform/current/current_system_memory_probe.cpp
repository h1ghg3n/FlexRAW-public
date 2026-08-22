#include "current_system_memory_probe.h"

#if defined(FLEXRAW_PLATFORM_CURRENT_WINDOWS)
#include "windows_system_memory_probe.h"
#elif defined(FLEXRAW_PLATFORM_CURRENT_LINUX)
#include "linux_system_memory_probe.h"
#else
#error "A current Platform memory probe adapter must be selected by CMake."
#endif

namespace flexraw::platform
{

// 목적: 현재 CMake target OS에 맞는 system memory probe 생성
// 입력: 없음
// 출력: application bootstrap이 소유할 concrete OS Adapter
std::unique_ptr<ISystemMemoryProbe> createCurrentSystemMemoryProbe()
{
#if defined(FLEXRAW_PLATFORM_CURRENT_WINDOWS)
    return std::make_unique<WindowsSystemMemoryProbe>();
#elif defined(FLEXRAW_PLATFORM_CURRENT_LINUX)
    return std::make_unique<LinuxSystemMemoryProbe>();
#endif
}

}  // namespace flexraw::platform

#include "current_process_memory_probe.h"

#if defined(FLEXRAW_PLATFORM_CURRENT_WINDOWS)
#include "windows_process_memory_probe.h"
#elif defined(FLEXRAW_PLATFORM_CURRENT_LINUX)
#include "linux_process_memory_probe.h"
#else
#error "A current Platform process memory probe adapter must be selected by CMake."
#endif

namespace flexraw::platform
{

// 목적: 현재 CMake target OS에 맞는 process memory probe 생성
// 입력: 없음
// 출력: application 또는 benchmark bootstrap이 소유할 concrete OS Adapter
std::unique_ptr<IProcessMemoryProbe> createCurrentProcessMemoryProbe()
{
#if defined(FLEXRAW_PLATFORM_CURRENT_WINDOWS)
    return std::make_unique<WindowsProcessMemoryProbe>();
#elif defined(FLEXRAW_PLATFORM_CURRENT_LINUX)
    return std::make_unique<LinuxProcessMemoryProbe>();
#endif
}

}  // namespace flexraw::platform

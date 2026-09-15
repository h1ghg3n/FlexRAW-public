#include "current_path_identity_service.h"

#if defined(FLEXRAW_PLATFORM_CURRENT_WINDOWS)
#include "windows_path_identity_service.h"
#elif defined(FLEXRAW_PLATFORM_CURRENT_LINUX)
#include "linux_path_identity_service.h"
#else
#error "A current Platform path identity adapter must be selected by CMake."
#endif

namespace flexraw::platform
{

// 목적: 현재 CMake target OS에 맞는 transient path identity service 생성
// 입력: 없음
// 출력: application bootstrap이 소유할 concrete OS Adapter
std::unique_ptr<IPathIdentityService> createCurrentPathIdentityService()
{
#if defined(FLEXRAW_PLATFORM_CURRENT_WINDOWS)
    return std::make_unique<WindowsPathIdentityService>();
#elif defined(FLEXRAW_PLATFORM_CURRENT_LINUX)
    return std::make_unique<LinuxPathIdentityService>();
#endif
}

}  // namespace flexraw::platform

#pragma once

#include <memory>

#include "path_identity_service.h"

namespace flexraw::platform
{

// 목적: 현재 CMake target OS에 맞는 transient path identity service 생성
// 입력: 없음
// 출력: application bootstrap이 소유할 concrete OS Adapter
[[nodiscard]] std::unique_ptr<IPathIdentityService> createCurrentPathIdentityService();

}  // namespace flexraw::platform

#pragma once

#include "path_identity_service.h"

namespace flexraw::platform
{

class WindowsPathIdentityService final : public IPathIdentityService
{
public:
    // 목적: Windows parent identity와 directory case policy를 반영한 임시 path key 생성
    // 입력: absolutePath: 기존 parent directory와 prospective leaf를 가진 절대 경로
    // 출력: 같은 parent와 Windows filename 의미를 비교할 key, native 조회 실패 시 nullopt
    [[nodiscard]] std::optional<TransientPathKey> comparisonKey(
        const std::filesystem::path& absolutePath) const override;
};

}  // namespace flexraw::platform

#pragma once

#include "path_identity_service.h"

namespace flexraw::platform
{

class LinuxPathIdentityService final : public IPathIdentityService
{
public:
    // 목적: Linux parent identity와 exact filename을 결합한 임시 path key 생성
    // 입력: absolutePath: 기존 parent directory와 prospective leaf를 가진 절대 경로
    // 출력: 같은 parent와 exact leaf를 비교할 key, native 조회 실패나 casefold parent면 nullopt
    [[nodiscard]] std::optional<TransientPathKey> comparisonKey(
        const std::filesystem::path& absolutePath) const override;
};

}  // namespace flexraw::platform

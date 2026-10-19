#pragma once

#include "display_profile_service.h"

namespace flexraw::platform
{

class WindowsDisplayProfileService final : public IDisplayProfileService
{
public:
    // 목적: Windows active monitor를 process-local display target snapshot으로 열거
    // 입력: 없음
    // 출력: 열거 성공 시 target snapshot 목록, Win32 query 실패 시 nullopt
    [[nodiscard]] std::optional<std::vector<DisplayTargetSnapshot>> displayTargets() const override;

    // 목적: Windows display device context의 현재 SDR ICC profile을 bounded byte snapshot으로 조회
    // 입력: targetId: 같은 service가 열거한 Windows display target identity
    // 출력: ICC payload 또는 profile/target/query/read 상태를 구분한 unavailable reason
    [[nodiscard]] DisplayProfileResult displayProfile(const DisplayTargetId& targetId) const override;
};

}  // namespace flexraw::platform

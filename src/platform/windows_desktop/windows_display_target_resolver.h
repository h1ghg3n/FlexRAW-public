#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "display_profile_service.h"

class QScreen;

namespace flexraw::platform
{

// 목적: Windows native display device name과 정확히 하나 일치하는 opaque target identity 선택
// 입력: deviceName: MONITORINFOEX device name, targets: 같은 profile service가 열거한 active target 목록
// 출력: unique exact match 또는 unknown/duplicate identity이면 빈 값
[[nodiscard]] std::optional<DisplayTargetId> matchWindowsDisplayTargetId(
    std::string_view deviceName, std::span<const DisplayTargetSnapshot> targets);

// 목적: Qt Windows screen의 native monitor를 기존 Platform display target identity로 대응
// 입력: screen: QWindow가 선택한 current screen, service: 같은 process의 display profile service
// 출력: unique target identity 또는 native handle/query/mapping 실패 시 빈 값
[[nodiscard]] std::optional<DisplayTargetId> resolveWindowsDisplayTarget(const QScreen& screen,
                                                                         const IDisplayProfileService& service);

}  // namespace flexraw::platform

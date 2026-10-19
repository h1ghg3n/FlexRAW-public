#pragma once

#include <cstdint>

#include "develop_params.h"

namespace flexraw::core::develop::detail
{

// 목적: point-local Develop이 사용하는 linear-light→8-bit sRGB projection 직접 실행
// 입력: linearValue — finite linear-light channel 값
// 출력: 기존 scalar reference와 같은 0~255 channel 값
[[nodiscard]] std::uint8_t projectLinearToSrgbChannel(float linearValue);

// 목적: 현재 parameter 집합이 실제 Develop pixel 처리를 요구하는지 판정
// 입력: params: 검증 전후 모두 가능한 Develop parameter 값
// 출력: 결과 pixel을 변경할 stage가 하나라도 활성화되면 true
[[nodiscard]] bool hasDevelopAdjustments(const types::DevelopParams& params) noexcept;

// 목적: 현재 parameter 집합이 neighborhood Develop stage를 요구하는지 판정
// 입력: params: 검증 전후 모두 가능한 Develop parameter 값
// 출력: NR, Clarity 또는 Sharpening이 활성화되면 true
[[nodiscard]] bool hasNeighborhoodAdjustments(const types::DevelopParams& params) noexcept;

}  // namespace flexraw::core::develop::detail

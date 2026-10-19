#pragma once

#include <cstdint>

#include "hsl_params.h"

namespace flexraw::core::develop::detail
{

// 목적: encoded sRGB pixel에 validated 8채널 scalar HSL 보정 적용
// 입력: pixel — RGB를 포함하는 최소 3 byte, params — [-1, 1] channel 보정값
// 출력: RGB만 제자리 갱신하며 alpha와 무채색은 보존
void applyHslToSrgbPixel(std::uint8_t* pixel, const types::HslParams& params);

}  // namespace flexraw::core::develop::detail

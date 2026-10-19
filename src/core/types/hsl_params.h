#pragma once

#include <array>

namespace flexraw::core::types
{

// 저장 및 wire 순서: red, orange, yellow, green, aqua, blue, purple, magenta.
struct HslChannelAdjustment
{
    float hue{0.0F};
    float saturation{0.0F};
    float luminance{0.0F};

    // 목적: HSL channel의 모든 보정값 비교
    // 입력: other — 비교할 channel 값
    // 출력: 모든 값이 같으면 true
    bool operator==(const HslChannelAdjustment& other) const = default;
};

struct HslParams
{
    std::array<HslChannelAdjustment, 8> channels{};

    // 목적: HSL recipe 전체의 값 비교
    // 입력: other — 비교할 recipe
    // 출력: 모든 channel 값이 같으면 true
    bool operator==(const HslParams& other) const = default;
};

}  // namespace flexraw::core::types

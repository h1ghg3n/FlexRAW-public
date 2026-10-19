#include "hsl_processing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace flexraw::core::develop::detail
{
namespace
{
constexpr std::array<float, 9> HueCenters{0.0F, 30.0F, 60.0F, 120.0F, 180.0F, 240.0F, 270.0F, 300.0F, 360.0F};

// 목적: 인접 channel 경계와 저채도 전이의 기울기를 연속으로 유지
// 입력: value — [0, 1] 보간 위치
// 출력: 양 끝 기울기가 0인 보간 weight
[[nodiscard]] float smoothWeight(float value)
{
    return value * value * (3.0F - 2.0F * value);
}

// 목적: 원본 hue 위치에서 인접 두 channel의 보정값을 한 번 보간
// 입력: hue — [0, 360) 원본 hue, params — validated recipe
// 출력: channel 처리 순서에 의존하지 않는 보정값
[[nodiscard]] types::HslChannelAdjustment adjustmentAtHue(float hue, const types::HslParams& params)
{
    std::size_t index = 0;
    while (index + 1 < params.channels.size() && hue >= HueCenters[index + 1])
    {
        ++index;
    }
    const float weight = smoothWeight((hue - HueCenters[index]) / (HueCenters[index + 1] - HueCenters[index]));
    const auto& left = params.channels[index];
    const auto& right = params.channels[(index + 1) % params.channels.size()];
    return {std::lerp(left.hue, right.hue, weight),
            std::lerp(left.saturation, right.saturation, weight),
            std::lerp(left.luminance, right.luminance, weight)};
}

// 목적: HSL을 encoded RGB로 변환하고 최종 byte로 투영
// 입력: pixel — RGB destination, hue/saturation/lightness — 정규화된 HSL
// 출력: lround 규칙으로 RGB byte 갱신
void writeHslPixel(std::uint8_t* pixel, float hue, float saturation, float lightness)
{
    const float chroma = (1.0F - std::abs(2.0F * lightness - 1.0F)) * saturation;
    const float sector = hue / 60.0F;
    const float secondary = chroma * (1.0F - std::abs(std::fmod(sector, 2.0F) - 1.0F));
    const float offset = lightness - chroma * 0.5F;
    const std::array<std::array<float, 3>, 6> sectors{{
        {chroma, secondary, 0.0F},
        {secondary, chroma, 0.0F},
        {0.0F, chroma, secondary},
        {0.0F, secondary, chroma},
        {secondary, 0.0F, chroma},
        {chroma, 0.0F, secondary},
    }};
    const auto& rgb = sectors[std::min(static_cast<std::size_t>(sector), sectors.size() - 1)];
    for (std::size_t component = 0; component < rgb.size(); ++component)
    {
        pixel[component] =
            static_cast<std::uint8_t>(std::lround(std::clamp(rgb[component] + offset, 0.0F, 1.0F) * 255.0F));
    }
}
}  // namespace

void applyHslToSrgbPixel(std::uint8_t* pixel, const types::HslParams& params)
{
    const float red = static_cast<float>(pixel[0]) / 255.0F;
    const float green = static_cast<float>(pixel[1]) / 255.0F;
    const float blue = static_cast<float>(pixel[2]) / 255.0F;
    const float maximum = std::max({red, green, blue});
    const float minimum = std::min({red, green, blue});
    const float chroma = maximum - minimum;
    if (chroma == 0.0F)
    {
        return;
    }
    float hue = maximum == red ? (green - blue) / chroma
                               : (maximum == green ? (blue - red) / chroma + 2.0F : (red - green) / chroma + 4.0F);
    hue *= 60.0F;
    if (hue < 0.0F)
    {
        hue += 360.0F;
    }
    const auto adjustment = adjustmentAtHue(hue, params);
    if (adjustment == types::HslChannelAdjustment{})
    {
        return;
    }
    const float protection = smoothWeight(std::min(chroma / 0.1F, 1.0F));
    const float lightness = (maximum + minimum) * 0.5F;
    const float saturation = chroma / (1.0F - std::abs(2.0F * lightness - 1.0F));
    const float adjustedHue = std::fmod(hue + 30.0F * adjustment.hue * protection + 360.0F, 360.0F);
    writeHslPixel(pixel,
                  adjustedHue,
                  std::clamp(saturation * (1.0F + adjustment.saturation * protection), 0.0F, 1.0F),
                  std::clamp(lightness * (1.0F + adjustment.luminance * protection), 0.0F, 1.0F));
}

}  // namespace flexraw::core::develop::detail

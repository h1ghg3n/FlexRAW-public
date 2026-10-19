#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace flexraw::core::measurement
{

struct Rgba8DifferenceMetrics
{
    std::uint64_t comparedPixelCount{0};
    double meanAbsoluteError{0.0};
    double rootMeanSquareError{0.0};
    double peakSignalToNoiseRatioDb{0.0};
    std::uint8_t maximumAbsoluteError{0};
};

// 목적: 같은 geometry의 RGBA8 image 두 장에서 RGB channel 오차 측정
// 입력: reference/candidate: pixel당 R,G,B,A 순서의 연속 byte buffer
// 출력: RGB 오차 지표 또는 크기/layout이 잘못됐으면 빈 값
[[nodiscard]] std::optional<Rgba8DifferenceMetrics> measureRgba8Difference(
    std::span<const std::uint8_t> reference, std::span<const std::uint8_t> candidate) noexcept;

}  // namespace flexraw::core::measurement

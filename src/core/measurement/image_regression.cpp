#include "image_regression.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace flexraw::core::measurement
{

// 목적: 같은 geometry의 RGBA8 image 두 장에서 RGB channel 오차 측정
// 입력: reference/candidate: pixel당 R,G,B,A 순서의 연속 byte buffer
// 출력: RGB 오차 지표 또는 크기/layout이 잘못됐으면 빈 값
std::optional<Rgba8DifferenceMetrics> measureRgba8Difference(const std::span<const std::uint8_t> reference,
                                                             const std::span<const std::uint8_t> candidate) noexcept
{
    constexpr std::size_t ChannelsPerPixel = 4;
    constexpr std::size_t ComparedChannelsPerPixel = 3;
    constexpr double MaximumChannelValue = 255.0;

    if (reference.empty() || reference.size() != candidate.size() || reference.size() % ChannelsPerPixel != 0)
    {
        return std::nullopt;
    }

    std::uint64_t absoluteErrorSum = 0;
    std::uint64_t squaredErrorSum = 0;
    std::uint8_t maximumAbsoluteError = 0;
    for (std::size_t pixelOffset = 0; pixelOffset < reference.size(); pixelOffset += ChannelsPerPixel)
    {
        for (std::size_t channel = 0; channel < ComparedChannelsPerPixel; ++channel)
        {
            const int difference =
                static_cast<int>(candidate[pixelOffset + channel]) - static_cast<int>(reference[pixelOffset + channel]);
            const auto absoluteError = static_cast<std::uint8_t>(std::abs(difference));
            absoluteErrorSum += absoluteError;
            squaredErrorSum += static_cast<std::uint64_t>(difference * difference);
            maximumAbsoluteError = std::max(maximumAbsoluteError, absoluteError);
        }
    }

    const std::uint64_t pixelCount = reference.size() / ChannelsPerPixel;
    const double sampleCount = static_cast<double>(pixelCount * ComparedChannelsPerPixel);
    const double meanAbsoluteError = static_cast<double>(absoluteErrorSum) / sampleCount;
    const double rootMeanSquareError = std::sqrt(static_cast<double>(squaredErrorSum) / sampleCount);
    const double peakSignalToNoiseRatio = rootMeanSquareError == 0.0
                                              ? std::numeric_limits<double>::infinity()
                                              : 20.0 * std::log10(MaximumChannelValue / rootMeanSquareError);

    return Rgba8DifferenceMetrics{
        pixelCount,
        meanAbsoluteError,
        rootMeanSquareError,
        peakSignalToNoiseRatio,
        maximumAbsoluteError,
    };
}

}  // namespace flexraw::core::measurement

#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>

#include "bounded_float32_neighborhood_detail.h"

namespace flexraw::core::develop::detail
{
namespace
{

constexpr float RedLuminanceWeight = 0.2126F;
constexpr float GreenLuminanceWeight = 0.7152F;
constexpr float BlueLuminanceWeight = 0.0722F;
constexpr float MinimumLuminanceForScaling = 0.00001F;

// 목적: image geometry에 필요한 pixel 수를 overflow 없이 계산
// 입력: width/rowCount — region geometry
// 출력: row-major pixel 수
[[nodiscard]] std::size_t checkedPixelCount(int width, int rowCount)
{
    if (width <= 0 || rowCount <= 0)
    {
        throw std::invalid_argument("Float32 neighborhood region geometry must be positive.");
    }

    const std::size_t unsignedWidth = static_cast<std::size_t>(width);
    const std::size_t unsignedRows = static_cast<std::size_t>(rowCount);
    if (unsignedRows > std::numeric_limits<std::size_t>::max() / unsignedWidth)
    {
        throw std::length_error("Float32 neighborhood region geometry overflows size_t.");
    }
    return unsignedWidth * unsignedRows;
}

// 목적: global row 좌표를 사용하는 Float32 region pixel offset 계산
// 입력: region과 global row/column 좌표
// 출력: region vector의 pixel index
[[nodiscard]] std::size_t regionOffset(const Float32Region& region, int row, int column)
{
    return static_cast<std::size_t>(row - region.firstRow) * static_cast<std::size_t>(region.width) +
           static_cast<std::size_t>(column);
}

// 목적: immutable Float32 region pixel 조회
// 입력: region과 global row/column 좌표
// 출력: 해당 linear RGB pixel 참조
[[nodiscard]] const LinearRgb& regionPixel(const Float32Region& region, int row, int column)
{
    return region.pixels[regionOffset(region, row, column)];
}

// 목적: mutable Float32 region pixel 조회
// 입력: region과 global row/column 좌표
// 출력: 해당 linear RGB pixel 참조
[[nodiscard]] LinearRgb& regionPixel(Float32Region& region, int row, int column)
{
    return region.pixels[regionOffset(region, row, column)];
}

// 목적: linear RGB pixel의 Rec.709 luminance 계산
// 입력: pixel — linear RGB 값
// 출력: linear-light luminance
[[nodiscard]] float luminance(const LinearRgb& pixel)
{
    return pixel.red * RedLuminanceWeight + pixel.green * GreenLuminanceWeight + pixel.blue * BlueLuminanceWeight;
}

}  // namespace

[[nodiscard]] Float32Region applyNoiseReduction(const Float32Region& source,
                                                int imageHeight,
                                                int firstRow,
                                                int lastRow,
                                                float luminanceStrength,
                                                float colorStrength)
{
    Float32Region result;
    result.width = source.width;
    result.firstRow = firstRow;
    result.rowCount = lastRow - firstRow;
    result.pixels.resize(checkedPixelCount(result.width, result.rowCount));

    for (int row = firstRow; row < lastRow; ++row)
    {
        for (int column = 0; column < result.width; ++column)
        {
            const LinearRgb& sourcePixel = regionPixel(source, row, column);
            const float sourceLuminance = luminance(sourcePixel);
            LinearRgb localAverage{};
            float localLuminance = 0.0F;

            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, imageHeight - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, result.width - 1);
                    const LinearRgb& sample = regionPixel(source, sampleRow, sampleColumn);
                    localAverage.red += sample.red;
                    localAverage.green += sample.green;
                    localAverage.blue += sample.blue;
                    localLuminance += luminance(sample);
                }
            }

            localAverage.red /= 9.0F;
            localAverage.green /= 9.0F;
            localAverage.blue /= 9.0F;
            localLuminance /= 9.0F;
            const float targetLuminance = sourceLuminance + (localLuminance - sourceLuminance) * luminanceStrength;
            const float luminanceScale =
                sourceLuminance > MinimumLuminanceForScaling ? targetLuminance / sourceLuminance : 1.0F;
            LinearRgb destination{
                sourcePixel.red * luminanceScale,
                sourcePixel.green * luminanceScale,
                sourcePixel.blue * luminanceScale,
            };

            if (colorStrength != 0.0F)
            {
                destination.red +=
                    (localAverage.red - localLuminance - (sourcePixel.red - sourceLuminance)) * colorStrength;
                destination.green +=
                    (localAverage.green - localLuminance - (sourcePixel.green - sourceLuminance)) * colorStrength;
                destination.blue +=
                    (localAverage.blue - localLuminance - (sourcePixel.blue - sourceLuminance)) * colorStrength;
            }

            regionPixel(result, row, column) = {
                std::clamp(destination.red, 0.0F, 1.0F),
                std::clamp(destination.green, 0.0F, 1.0F),
                std::clamp(destination.blue, 0.0F, 1.0F),
            };
        }
    }
    return result;
}

}  // namespace flexraw::core::develop::detail

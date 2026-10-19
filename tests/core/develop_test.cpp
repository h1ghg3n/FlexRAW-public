#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include <QColor>
#include <QImage>
#include <QSize>

#include <gtest/gtest.h>

#include "develop.h"
#include "develop_processing.h"

namespace flexraw::core::develop
{
namespace
{

// 목적: 지정 RGBA 값으로 1 pixel test image 생성
// 입력: color: 생성할 pixel 색상
// 출력: RGBA8888 format의 1 pixel QImage
[[nodiscard]] QImage createPixelImage(const QColor& color)
{
    QImage image(1, 1, QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, color);
    return image;
}

// 목적: test reference에서 8-bit sRGB channel을 linear-light 값으로 변환
// 입력: channel: 0~255 범위의 sRGB channel 값
// 출력: IEC 61966-2-1 transfer function을 적용한 0~1 linear-light 값
[[nodiscard]] float referenceSrgbChannelToLinear(int channel)
{
    const float encodedValue = static_cast<float>(channel) / 255.0F;
    if (encodedValue <= 0.04045F)
    {
        return encodedValue / 12.92F;
    }
    return std::pow((encodedValue + 0.055F) / 1.055F, 2.4F);
}

// 목적: test reference에서 linear-light 값을 normalized sRGB로 변환
// 입력: linearValue — 변환할 linear-light 값
// 출력: IEC 61966-2-1 transfer function을 적용한 normalized 값
[[nodiscard]] float referenceLinearToSrgb(float linearValue)
{
    return linearValue <= 0.0031308F ? linearValue * 12.92F : 1.055F * std::pow(linearValue, 1.0F / 2.4F) - 0.055F;
}

// 목적: test reference에서 linear-light 값을 8-bit sRGB channel로 변환
// 입력: linearValue — exposure가 적용된 linear-light 값
// 출력: IEC 61966-2-1 transfer function과 clamp/round를 적용한 channel 값
[[nodiscard]] int referenceLinearToSrgbChannel(float linearValue)
{
    return static_cast<int>(std::lround(std::clamp(referenceLinearToSrgb(linearValue), 0.0F, 1.0F) * 255.0F));
}

// 목적: 기존 Clarity 경로와 동일한 linear-light 휘도 계산
// 입력: pixel: RGBA8888 source pixel
// 출력: 0~1 범위의 linear-light 휘도
[[nodiscard]] float referenceClarityLuminance(const uchar* pixel)
{
    return referenceSrgbChannelToLinear(pixel[0]) * 0.2126F + referenceSrgbChannelToLinear(pixel[1]) * 0.7152F +
           referenceSrgbChannelToLinear(pixel[2]) * 0.0722F;
}

// 목적: cache 이전의 3x3 Clarity 계산을 독립적인 pixel reference로 재현
// 입력: source: RGBA8888 image, clarity: 적용할 보정량
// 출력: 원본 alpha를 보존한 기준 image
[[nodiscard]] QImage referenceClarityImage(const QImage& source, float clarity)
{
    QImage result = source.copy();
    for (int row = 0; row < source.height(); ++row)
    {
        uchar* destinationPixels = result.scanLine(row);
        for (int column = 0; column < source.width(); ++column)
        {
            const uchar* sourcePixel = source.constScanLine(row) + column * 4;
            const float luminance = referenceClarityLuminance(sourcePixel);
            float localLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, source.height() - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, source.width() - 1);
                    localLuminance += referenceClarityLuminance(source.constScanLine(sampleRow) + sampleColumn * 4);
                }
            }

            localLuminance /= 9.0F;
            const float midtoneWeight = 1.0F - std::abs(luminance * 2.0F - 1.0F);
            const float adjustedLuminance =
                std::clamp(luminance + (luminance - localLuminance) * clarity * midtoneWeight, 0.0F, 1.0F);
            const float luminanceScale = luminance > 0.00001F ? adjustedLuminance / luminance : 1.0F;
            uchar* destinationPixel = destinationPixels + column * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                destinationPixel[channel] = static_cast<uchar>(
                    referenceLinearToSrgbChannel(referenceSrgbChannelToLinear(sourcePixel[channel]) * luminanceScale));
            }
        }
    }
    return result;
}

// 목적: cache 이전 Noise Reduction의 3x3 RGB·휘도 누산을 독립적으로 재현
// 입력: source: RGBA8888 image, luminanceStrength/colorStrength: 적용할 감쇄 강도
// 출력: 원본 alpha와 경계 clamp를 유지한 기준 image
[[nodiscard]] QImage referenceNoiseReductionImage(const QImage& source, float luminanceStrength, float colorStrength)
{
    QImage result = source.copy();
    for (int row = 0; row < source.height(); ++row)
    {
        uchar* destinationPixels = result.scanLine(row);
        for (int column = 0; column < source.width(); ++column)
        {
            const uchar* sourcePixel = source.constScanLine(row) + column * 4;
            const float sourceRed = referenceSrgbChannelToLinear(sourcePixel[0]);
            const float sourceGreen = referenceSrgbChannelToLinear(sourcePixel[1]);
            const float sourceBlue = referenceSrgbChannelToLinear(sourcePixel[2]);
            const float sourceLuminance = sourceRed * 0.2126F + sourceGreen * 0.7152F + sourceBlue * 0.0722F;
            float averageRed = 0.0F;
            float averageGreen = 0.0F;
            float averageBlue = 0.0F;
            float localLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, source.height() - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, source.width() - 1);
                    const uchar* samplePixel = source.constScanLine(sampleRow) + sampleColumn * 4;
                    const float red = referenceSrgbChannelToLinear(samplePixel[0]);
                    const float green = referenceSrgbChannelToLinear(samplePixel[1]);
                    const float blue = referenceSrgbChannelToLinear(samplePixel[2]);
                    averageRed += red;
                    averageGreen += green;
                    averageBlue += blue;
                    localLuminance += red * 0.2126F + green * 0.7152F + blue * 0.0722F;
                }
            }

            averageRed /= 9.0F;
            averageGreen /= 9.0F;
            averageBlue /= 9.0F;
            localLuminance /= 9.0F;
            const float targetLuminance = sourceLuminance + (localLuminance - sourceLuminance) * luminanceStrength;
            const float luminanceScale = sourceLuminance > 0.00001F ? targetLuminance / sourceLuminance : 1.0F;
            float destinationRed = sourceRed * luminanceScale;
            float destinationGreen = sourceGreen * luminanceScale;
            float destinationBlue = sourceBlue * luminanceScale;
            if (colorStrength != 0.0F)
            {
                destinationRed += (averageRed - localLuminance - (sourceRed - sourceLuminance)) * colorStrength;
                destinationGreen += (averageGreen - localLuminance - (sourceGreen - sourceLuminance)) * colorStrength;
                destinationBlue += (averageBlue - localLuminance - (sourceBlue - sourceLuminance)) * colorStrength;
            }

            uchar* destinationPixel = destinationPixels + column * 4;
            destinationPixel[0] = static_cast<uchar>(referenceLinearToSrgbChannel(destinationRed));
            destinationPixel[1] = static_cast<uchar>(referenceLinearToSrgbChannel(destinationGreen));
            destinationPixel[2] = static_cast<uchar>(referenceLinearToSrgbChannel(destinationBlue));
        }
    }
    return result;
}

struct ReferenceLinearPixel
{
    float red{0.0F};
    float green{0.0F};
    float blue{0.0F};
};

// 목적: Float32 reference pixel의 Rec.709 luminance 계산
// 입력: pixel — linear RGB 값
// 출력: linear-light luminance
[[nodiscard]] float referenceLinearLuminance(const ReferenceLinearPixel& pixel)
{
    return pixel.red * 0.2126F + pixel.green * 0.7152F + pixel.blue * 0.0722F;
}

// 목적: NR 뒤 중간 양자화 없이 Clarity까지 유지하는 full-frame Float32 reference 계산
// 입력: source와 NR/Clarity 보정 강도
// 출력: 최종 RGBA8 projection과 원본 alpha를 가진 기준 image
[[nodiscard]] QImage referenceFloat32NoiseReductionThenClarityImage(const QImage& source,
                                                                    float luminanceStrength,
                                                                    float colorStrength,
                                                                    float clarity)
{
    const int width = source.width();
    const int height = source.height();
    const auto offset = [width](int row, int column) {
        return static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column);
    };
    std::vector<ReferenceLinearPixel> linear(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<ReferenceLinearPixel> noise(linear.size());

    for (int row = 0; row < height; ++row)
    {
        const uchar* sourcePixels = source.constScanLine(row);
        for (int column = 0; column < width; ++column)
        {
            const uchar* pixel = sourcePixels + column * 4;
            linear[offset(row, column)] = {
                referenceSrgbChannelToLinear(pixel[0]),
                referenceSrgbChannelToLinear(pixel[1]),
                referenceSrgbChannelToLinear(pixel[2]),
            };
        }
    }

    for (int row = 0; row < height; ++row)
    {
        for (int column = 0; column < width; ++column)
        {
            const ReferenceLinearPixel& sourcePixel = linear[offset(row, column)];
            const float sourceLuminance = referenceLinearLuminance(sourcePixel);
            ReferenceLinearPixel average{};
            float averageLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, height - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, width - 1);
                    const ReferenceLinearPixel& sample = linear[offset(sampleRow, sampleColumn)];
                    average.red += sample.red;
                    average.green += sample.green;
                    average.blue += sample.blue;
                    averageLuminance += referenceLinearLuminance(sample);
                }
            }

            average.red /= 9.0F;
            average.green /= 9.0F;
            average.blue /= 9.0F;
            averageLuminance /= 9.0F;
            const float targetLuminance = sourceLuminance + (averageLuminance - sourceLuminance) * luminanceStrength;
            const float scale = sourceLuminance > 0.00001F ? targetLuminance / sourceLuminance : 1.0F;
            ReferenceLinearPixel destination{
                sourcePixel.red * scale,
                sourcePixel.green * scale,
                sourcePixel.blue * scale,
            };
            destination.red += (average.red - averageLuminance - (sourcePixel.red - sourceLuminance)) * colorStrength;
            destination.green +=
                (average.green - averageLuminance - (sourcePixel.green - sourceLuminance)) * colorStrength;
            destination.blue +=
                (average.blue - averageLuminance - (sourcePixel.blue - sourceLuminance)) * colorStrength;
            noise[offset(row, column)] = {
                std::clamp(destination.red, 0.0F, 1.0F),
                std::clamp(destination.green, 0.0F, 1.0F),
                std::clamp(destination.blue, 0.0F, 1.0F),
            };
        }
    }

    QImage result = source.copy();
    for (int row = 0; row < height; ++row)
    {
        uchar* destinationPixels = result.scanLine(row);
        for (int column = 0; column < width; ++column)
        {
            const ReferenceLinearPixel& sourcePixel = noise[offset(row, column)];
            const float sourceLuminance = referenceLinearLuminance(sourcePixel);
            float localLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, height - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, width - 1);
                    localLuminance += referenceLinearLuminance(noise[offset(sampleRow, sampleColumn)]);
                }
            }

            localLuminance /= 9.0F;
            const float midtoneWeight = 1.0F - std::abs(sourceLuminance * 2.0F - 1.0F);
            const float adjustedLuminance =
                std::clamp(sourceLuminance + (sourceLuminance - localLuminance) * clarity * midtoneWeight, 0.0F, 1.0F);
            const float scale = sourceLuminance > 0.00001F ? adjustedLuminance / sourceLuminance : 1.0F;
            uchar* destinationPixel = destinationPixels + column * 4;
            destinationPixel[0] = static_cast<uchar>(referenceLinearToSrgbChannel(sourcePixel.red * scale));
            destinationPixel[1] = static_cast<uchar>(referenceLinearToSrgbChannel(sourcePixel.green * scale));
            destinationPixel[2] = static_cast<uchar>(referenceLinearToSrgbChannel(sourcePixel.blue * scale));
        }
    }
    return result;
}

// 목적: full-frame reference로 Float32 NR→Clarity→Sharpening chain 독립 계산
// 입력: source — RGBA8888 stage 입력, params — spatial Develop parameter
// 출력: 중간 byte 양자화 없이 계산한 최종 RGBA8 image
[[nodiscard]] QImage referenceFloat32SpatialChainImage(const QImage& source, const types::DevelopParams& params)
{
    const int width = source.width();
    const int height = source.height();
    const auto offset = [width](int row, int column) {
        return static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + static_cast<std::size_t>(column);
    };
    std::vector<ReferenceLinearPixel> linear(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<ReferenceLinearPixel> noise(linear.size());
    std::vector<ReferenceLinearPixel> encoded(linear.size());

    for (int row = 0; row < height; ++row)
    {
        const uchar* sourcePixels = source.constScanLine(row);
        for (int column = 0; column < width; ++column)
        {
            const uchar* pixel = sourcePixels + column * 4;
            linear[offset(row, column)] = {
                referenceSrgbChannelToLinear(pixel[0]),
                referenceSrgbChannelToLinear(pixel[1]),
                referenceSrgbChannelToLinear(pixel[2]),
            };
        }
    }

    for (int row = 0; row < height; ++row)
    {
        for (int column = 0; column < width; ++column)
        {
            const ReferenceLinearPixel& sourcePixel = linear[offset(row, column)];
            const float sourceLuminance = referenceLinearLuminance(sourcePixel);
            ReferenceLinearPixel average{};
            float averageLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, height - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, width - 1);
                    const ReferenceLinearPixel& sample = linear[offset(sampleRow, sampleColumn)];
                    average.red += sample.red;
                    average.green += sample.green;
                    average.blue += sample.blue;
                    averageLuminance += referenceLinearLuminance(sample);
                }
            }

            average.red /= 9.0F;
            average.green /= 9.0F;
            average.blue /= 9.0F;
            averageLuminance /= 9.0F;
            const float targetLuminance =
                sourceLuminance + (averageLuminance - sourceLuminance) * params.luminanceNoiseReduction;
            const float scale = sourceLuminance > 0.00001F ? targetLuminance / sourceLuminance : 1.0F;
            ReferenceLinearPixel destination{
                sourcePixel.red * scale,
                sourcePixel.green * scale,
                sourcePixel.blue * scale,
            };
            destination.red +=
                (average.red - averageLuminance - (sourcePixel.red - sourceLuminance)) * params.colorNoiseReduction;
            destination.green +=
                (average.green - averageLuminance - (sourcePixel.green - sourceLuminance)) * params.colorNoiseReduction;
            destination.blue +=
                (average.blue - averageLuminance - (sourcePixel.blue - sourceLuminance)) * params.colorNoiseReduction;
            noise[offset(row, column)] = {
                std::clamp(destination.red, 0.0F, 1.0F),
                std::clamp(destination.green, 0.0F, 1.0F),
                std::clamp(destination.blue, 0.0F, 1.0F),
            };
        }
    }

    for (int row = 0; row < height; ++row)
    {
        for (int column = 0; column < width; ++column)
        {
            const ReferenceLinearPixel& sourcePixel = noise[offset(row, column)];
            const float sourceLuminance = referenceLinearLuminance(sourcePixel);
            float localLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, height - 1);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, width - 1);
                    localLuminance += referenceLinearLuminance(noise[offset(sampleRow, sampleColumn)]);
                }
            }

            localLuminance /= 9.0F;
            const float midtoneWeight = 1.0F - std::abs(sourceLuminance * 2.0F - 1.0F);
            const float adjustedLuminance = std::clamp(
                sourceLuminance + (sourceLuminance - localLuminance) * params.clarity * midtoneWeight, 0.0F, 1.0F);
            const float scale = sourceLuminance > 0.00001F ? adjustedLuminance / sourceLuminance : 1.0F;
            const ReferenceLinearPixel adjusted{
                sourcePixel.red * scale,
                sourcePixel.green * scale,
                sourcePixel.blue * scale,
            };
            encoded[offset(row, column)] = {
                std::clamp(referenceLinearToSrgb(adjusted.red), 0.0F, 1.0F),
                std::clamp(referenceLinearToSrgb(adjusted.green), 0.0F, 1.0F),
                std::clamp(referenceLinearToSrgb(adjusted.blue), 0.0F, 1.0F),
            };
        }
    }

    QImage result = source.copy();
    const int radius = static_cast<int>(std::lround(params.sharpeningRadius));
    const int broadSampleCount = (radius * 2 + 1) * (radius * 2 + 1);
    const float detailWeight = (params.sharpeningDetail + 1.0F) * 0.5F;
    for (int row = 0; row < height; ++row)
    {
        uchar* destinationPixels = result.scanLine(row);
        for (int column = 0; column < width; ++column)
        {
            const ReferenceLinearPixel& center = encoded[offset(row, column)];
            float maximumDifference = 0.0F;
            for (const auto [sampleRow, sampleColumn] : {std::pair{row, std::max(column - 1, 0)},
                                                         std::pair{row, std::min(column + 1, width - 1)},
                                                         std::pair{std::max(row - 1, 0), column},
                                                         std::pair{std::min(row + 1, height - 1), column}})
            {
                const ReferenceLinearPixel& sample = encoded[offset(sampleRow, sampleColumn)];
                maximumDifference = std::max(maximumDifference, std::abs(center.red - sample.red));
                maximumDifference = std::max(maximumDifference, std::abs(center.green - sample.green));
                maximumDifference = std::max(maximumDifference, std::abs(center.blue - sample.blue));
            }
            const float edgeWeight = 1.0F - params.sharpeningMasking * (1.0F - maximumDifference);

            const std::array<float, 3> centerChannels{center.red, center.green, center.blue};
            std::array<float, 3> broadSum{};
            std::array<float, 3> fineSum{};
            for (int rowOffset = -radius; rowOffset <= radius; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, height - 1);
                for (int columnOffset = -radius; columnOffset <= radius; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, width - 1);
                    const ReferenceLinearPixel& sample = encoded[offset(sampleRow, sampleColumn)];
                    const std::array<float, 3> sampleChannels{sample.red, sample.green, sample.blue};
                    for (std::size_t channel = 0; channel < sampleChannels.size(); ++channel)
                    {
                        broadSum[channel] += sampleChannels[channel];
                        if (radius > 1 && std::abs(rowOffset) <= 1 && std::abs(columnOffset) <= 1)
                        {
                            fineSum[channel] += sampleChannels[channel];
                        }
                    }
                }
            }

            uchar* destinationPixel = destinationPixels + column * 4;
            for (std::size_t channel = 0; channel < centerChannels.size(); ++channel)
            {
                const float broadSignal =
                    centerChannels[channel] - broadSum[channel] / static_cast<float>(broadSampleCount);
                const float fineSignal = centerChannels[channel] - fineSum[channel] / 9.0F;
                const float signal =
                    radius == 1 ? broadSignal : broadSignal + (fineSignal - broadSignal) * detailWeight;
                const float value =
                    std::clamp(centerChannels[channel] + signal * params.sharpeningAmount * edgeWeight, 0.0F, 1.0F);
                destinationPixel[channel] = static_cast<uchar>(value * 255.0F + 0.5F);
            }
        }
    }
    return result;
}

// 목적: test reference에서 QColor의 RGB channel을 위치별로 읽기
// 입력: pixel: 원본 색상, channel: RGB channel index
// 출력: 지정 channel의 8-bit 값
[[nodiscard]] int referenceRgbChannel(const QColor& pixel, int channel)
{
    return channel == 0 ? pixel.red() : channel == 1 ? pixel.green() : pixel.blue();
}

// 목적: source 좌표 접근만 사용해 controlled Sharpening의 경계·masking 결과를 독립 계산
// 입력: source: RGBA8888 입력, params: Sharpening 제어값
// 출력: 원본 alpha와 stage별 8-bit rounding을 보존한 기준 image
[[nodiscard]] QImage referenceControlledSharpeningImage(const QImage& source, const types::DevelopParams& params)
{
    QImage result = source.copy();
    const int radius = static_cast<int>(std::lround(params.sharpeningRadius));
    const float detailWeight = (params.sharpeningDetail + 1.0F) * 0.5F;
    const int broadSampleCount = (radius * 2 + 1) * (radius * 2 + 1);
    for (int row = 0; row < source.height(); ++row)
    {
        for (int column = 0; column < source.width(); ++column)
        {
            const QColor center = source.pixelColor(column, row);
            float edgeWeight = 1.0F;
            if (params.sharpeningMasking != 0.0F)
            {
                const std::array<QColor, 4> neighbors{
                    source.pixelColor(std::max(column - 1, 0), row),
                    source.pixelColor(std::min(column + 1, source.width() - 1), row),
                    source.pixelColor(column, std::max(row - 1, 0)),
                    source.pixelColor(column, std::min(row + 1, source.height() - 1))};
                float maximumDifference = 0.0F;
                for (int channel = 0; channel < 3; ++channel)
                {
                    for (const QColor& neighbor : neighbors)
                    {
                        maximumDifference =
                            std::max(maximumDifference,
                                     std::abs(static_cast<float>(referenceRgbChannel(center, channel) -
                                                                 referenceRgbChannel(neighbor, channel))));
                    }
                }
                edgeWeight = 1.0F - params.sharpeningMasking * (1.0F - maximumDifference / 255.0F);
            }

            for (int channel = 0; channel < 3; ++channel)
            {
                int broadSum = 0;
                int fineSum = 0;
                for (int rowOffset = -radius; rowOffset <= radius; ++rowOffset)
                {
                    for (int columnOffset = -radius; columnOffset <= radius; ++columnOffset)
                    {
                        const QColor sample =
                            source.pixelColor(std::clamp(column + columnOffset, 0, source.width() - 1),
                                              std::clamp(row + rowOffset, 0, source.height() - 1));
                        const int value = referenceRgbChannel(sample, channel);
                        broadSum += value;
                        if (radius > 1 && std::abs(rowOffset) <= 1 && std::abs(columnOffset) <= 1)
                        {
                            fineSum += value;
                        }
                    }
                }

                const float sourceValue = static_cast<float>(referenceRgbChannel(center, channel));
                const float broadSignal =
                    sourceValue - static_cast<float>(broadSum) / static_cast<float>(broadSampleCount);
                const float fineSignal = sourceValue - static_cast<float>(fineSum) / 9.0F;
                const float signal =
                    radius == 1 ? broadSignal : broadSignal + (fineSignal - broadSignal) * detailWeight;
                result.scanLine(row)[column * 4 + channel] = static_cast<uchar>(
                    std::clamp(sourceValue + signal * params.sharpeningAmount * edgeWeight, 0.0F, 255.0F) + 0.5F);
            }
        }
    }
    return result;
}

TEST(Develop, PreservesImageForZeroExposure)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 90});
    const types::DevelopParams params{};

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(source.pixelColor(0, 0), result.value().pixelColor(0, 0));
}

TEST(Develop, AppliesExposureInLinearLight)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 90});
    const DevelopImageResult brighter = applyDevelop(source, types::DevelopParams{1.0F});
    const DevelopImageResult darker = applyDevelop(source, types::DevelopParams{-1.0F});

    ASSERT_TRUE(brighter.hasValue());
    ASSERT_TRUE(darker.hasValue());
    EXPECT_EQ(176, brighter.value().pixelColor(0, 0).red());
    EXPECT_EQ(92, darker.value().pixelColor(0, 0).red());
    EXPECT_EQ(90, brighter.value().pixelColor(0, 0).alpha());
    EXPECT_EQ(90, darker.value().pixelColor(0, 0).alpha());
}

TEST(Develop, PreservesExhaustiveEightBitSrgbTransferDuringExposure)
{
    constexpr int ChannelValueCount = 256;
    constexpr float ExposureEv = 0.35F;
    QImage source(ChannelValueCount, 1, QImage::Format_RGBA8888);
    uchar* sourcePixels = source.scanLine(0);
    for (int channel = 0; channel < ChannelValueCount; ++channel)
    {
        uchar* pixel = sourcePixels + channel * 4;
        pixel[0] = static_cast<uchar>(channel);
        pixel[1] = static_cast<uchar>(channel);
        pixel[2] = static_cast<uchar>(channel);
        pixel[3] = static_cast<uchar>(255 - channel);
    }

    types::DevelopParams params;
    params.exposureEv = ExposureEv;
    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    const uchar* resultPixels = result.value().constScanLine(0);
    const float exposureScale = std::exp2(ExposureEv);
    for (int channel = 0; channel < ChannelValueCount; ++channel)
    {
        const int expected = referenceLinearToSrgbChannel(referenceSrgbChannelToLinear(channel) * exposureScale);
        const uchar* pixel = resultPixels + channel * 4;
        EXPECT_EQ(expected, pixel[0]) << "red channel=" << channel;
        EXPECT_EQ(expected, pixel[1]) << "green channel=" << channel;
        EXPECT_EQ(expected, pixel[2]) << "blue channel=" << channel;
        EXPECT_EQ(255 - channel, pixel[3]) << "alpha channel=" << channel;
    }
}

TEST(Develop, ClipsOverexposedChannels)
{
    const QImage source = createPixelImage(QColor{255, 64, 0, 255});
    const DevelopImageResult result = applyDevelop(source, types::DevelopParams{4.0F});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(255, result.value().pixelColor(0, 0).red());
    EXPECT_EQ(0, result.value().pixelColor(0, 0).blue());
}

TEST(Develop, AdjustsContrastAroundLinearMiddleGray)
{
    const QImage shadows = createPixelImage(QColor{64, 64, 64, 90});
    const QImage highlights = createPixelImage(QColor{192, 192, 192, 90});
    const types::DevelopParams higherContrast{0.0F, 0.5F};
    const types::DevelopParams lowerContrast{0.0F, -0.5F};

    const DevelopImageResult darkerShadows = applyDevelop(shadows, higherContrast);
    const DevelopImageResult brighterHighlights = applyDevelop(highlights, higherContrast);
    const DevelopImageResult lighterShadows = applyDevelop(shadows, lowerContrast);
    const DevelopImageResult darkerHighlights = applyDevelop(highlights, lowerContrast);

    ASSERT_TRUE(darkerShadows.hasValue());
    ASSERT_TRUE(brighterHighlights.hasValue());
    ASSERT_TRUE(lighterShadows.hasValue());
    ASSERT_TRUE(darkerHighlights.hasValue());
    EXPECT_LT(darkerShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_GT(brighterHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_GT(lighterShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_LT(darkerHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_EQ(90, darkerShadows.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsShadowsWithoutChangingHighlights)
{
    const QImage shadows = createPixelImage(QColor{32, 32, 32, 90});
    const QImage highlights = createPixelImage(QColor{224, 224, 224, 90});
    const DevelopImageResult raisedShadows = applyDevelop(shadows, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.5F});
    const DevelopImageResult loweredShadows = applyDevelop(shadows, types::DevelopParams{0.0F, 0.0F, 0.0F, -0.5F});
    const DevelopImageResult unchangedHighlights =
        applyDevelop(highlights, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.5F});

    ASSERT_TRUE(raisedShadows.hasValue());
    ASSERT_TRUE(loweredShadows.hasValue());
    ASSERT_TRUE(unchangedHighlights.hasValue());
    EXPECT_GT(raisedShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_LT(loweredShadows.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_EQ(highlights.pixelColor(0, 0), unchangedHighlights.value().pixelColor(0, 0));
    EXPECT_EQ(90, raisedShadows.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsHighlightsWithoutChangingShadows)
{
    const QImage shadows = createPixelImage(QColor{32, 32, 32, 90});
    const QImage highlights = createPixelImage(QColor{224, 224, 224, 90});
    const DevelopImageResult raisedHighlights = applyDevelop(highlights, types::DevelopParams{0.0F, 0.0F, 0.5F, 0.0F});
    const DevelopImageResult loweredHighlights =
        applyDevelop(highlights, types::DevelopParams{0.0F, 0.0F, -0.5F, 0.0F});
    const DevelopImageResult unchangedShadows = applyDevelop(shadows, types::DevelopParams{0.0F, 0.0F, 0.5F, 0.0F});

    ASSERT_TRUE(raisedHighlights.hasValue());
    ASSERT_TRUE(loweredHighlights.hasValue());
    ASSERT_TRUE(unchangedShadows.hasValue());
    EXPECT_GT(raisedHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_LT(loweredHighlights.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_EQ(shadows.pixelColor(0, 0), unchangedShadows.value().pixelColor(0, 0));
    EXPECT_EQ(90, loweredHighlights.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsBlacksWithoutChangingWhites)
{
    const QImage blacks = createPixelImage(QColor{16, 16, 16, 90});
    const QImage whites = createPixelImage(QColor{240, 240, 240, 90});
    const DevelopImageResult raisedBlacks =
        applyDevelop(blacks, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F});
    const DevelopImageResult loweredBlacks =
        applyDevelop(blacks, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -0.5F});
    const DevelopImageResult unchangedWhites =
        applyDevelop(whites, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F});

    ASSERT_TRUE(raisedBlacks.hasValue());
    ASSERT_TRUE(loweredBlacks.hasValue());
    ASSERT_TRUE(unchangedWhites.hasValue());
    EXPECT_GT(raisedBlacks.value().pixelColor(0, 0).red(), blacks.pixelColor(0, 0).red());
    EXPECT_LT(loweredBlacks.value().pixelColor(0, 0).red(), blacks.pixelColor(0, 0).red());
    EXPECT_EQ(whites.pixelColor(0, 0), unchangedWhites.value().pixelColor(0, 0));
    EXPECT_EQ(90, raisedBlacks.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsWhitesWithoutChangingBlacks)
{
    const QImage blacks = createPixelImage(QColor{16, 16, 16, 90});
    const QImage whites = createPixelImage(QColor{240, 240, 240, 90});
    const DevelopImageResult raisedWhites =
        applyDevelop(whites, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F});
    const DevelopImageResult loweredWhites =
        applyDevelop(whites, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, -0.5F, 0.0F});
    const DevelopImageResult unchangedBlacks =
        applyDevelop(blacks, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F});

    ASSERT_TRUE(raisedWhites.hasValue());
    ASSERT_TRUE(loweredWhites.hasValue());
    ASSERT_TRUE(unchangedBlacks.hasValue());
    EXPECT_GT(raisedWhites.value().pixelColor(0, 0).red(), whites.pixelColor(0, 0).red());
    EXPECT_LT(loweredWhites.value().pixelColor(0, 0).red(), whites.pixelColor(0, 0).red());
    EXPECT_EQ(blacks.pixelColor(0, 0), unchangedBlacks.value().pixelColor(0, 0));
    EXPECT_EQ(90, loweredWhites.value().pixelColor(0, 0).alpha());
}

TEST(Develop, AdjustsSaturationAroundLinearLuminance)
{
    const QImage source = createPixelImage(QColor{160, 96, 32, 90});
    const DevelopImageResult desaturated =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -1.0F});
    const DevelopImageResult saturated =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.5F});

    ASSERT_TRUE(desaturated.hasValue());
    ASSERT_TRUE(saturated.hasValue());
    const QColor grayscale = desaturated.value().pixelColor(0, 0);
    const QColor moreSaturated = saturated.value().pixelColor(0, 0);
    EXPECT_EQ(grayscale.red(), grayscale.green());
    EXPECT_EQ(grayscale.green(), grayscale.blue());
    EXPECT_GT(moreSaturated.red(), source.pixelColor(0, 0).red());
    EXPECT_LT(moreSaturated.green(), source.pixelColor(0, 0).green());
    EXPECT_LT(moreSaturated.blue(), source.pixelColor(0, 0).blue());
    EXPECT_EQ(90, moreSaturated.alpha());
}

TEST(Develop, PreservesNeutralGrayForSaturation)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 90});
    const DevelopImageResult result =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.75F});

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(source.pixelColor(0, 0), result.value().pixelColor(0, 0));
}

TEST(Develop, PrioritizesMutedColorsForPositiveVibrance)
{
    const QImage mutedSource = createPixelImage(QColor{128, 112, 96, 90});
    const QImage saturatedSource = createPixelImage(QColor{160, 96, 32, 90});
    const types::DevelopParams vibrance{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.75F};

    const DevelopImageResult mutedResult = applyDevelop(mutedSource, vibrance);
    const DevelopImageResult saturatedResult = applyDevelop(saturatedSource, vibrance);

    ASSERT_TRUE(mutedResult.hasValue());
    ASSERT_TRUE(saturatedResult.hasValue());
    const QColor mutedColor = mutedResult.value().pixelColor(0, 0);
    const QColor saturatedColor = saturatedResult.value().pixelColor(0, 0);
    EXPECT_GT(mutedColor.red(), mutedSource.pixelColor(0, 0).red());
    EXPECT_LT(mutedColor.blue(), mutedSource.pixelColor(0, 0).blue());
    EXPECT_GT(mutedColor.red() - mutedSource.pixelColor(0, 0).red(),
              saturatedColor.red() - saturatedSource.pixelColor(0, 0).red());
    EXPECT_EQ(90, mutedColor.alpha());
}

TEST(Develop, ReducesChromaForNegativeVibrance)
{
    const QImage source = createPixelImage(QColor{160, 96, 32, 90});
    const DevelopImageResult result =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -0.5F});

    ASSERT_TRUE(result.hasValue());
    const QColor resultColor = result.value().pixelColor(0, 0);
    EXPECT_LT(resultColor.red(), source.pixelColor(0, 0).red());
    EXPECT_GT(resultColor.blue(), source.pixelColor(0, 0).blue());
    EXPECT_EQ(90, resultColor.alpha());
}

TEST(Develop, AppliesCustomWhiteBalanceWithoutChangingAlpha)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 90});
    types::DevelopParams params;
    params.whiteBalanceMode = types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 3200.0F;
    params.whiteBalanceTint = 0.4F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    const QColor adjusted = result.value().pixelColor(0, 0);
    EXPECT_EQ(adjusted.red(), source.pixelColor(0, 0).red());
    EXPECT_LT(adjusted.green(), source.pixelColor(0, 0).green());
    EXPECT_LT(adjusted.blue(), source.pixelColor(0, 0).blue());
    EXPECT_EQ(90, adjusted.alpha());
}

TEST(Develop, AdjustsMidtoneLocalContrastForClarity)
{
    QImage source(QSize(3, 3), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(1, 1, QColor(160, 160, 160, 90));
    types::DevelopParams positiveClarity;
    positiveClarity.clarity = 1.0F;
    types::DevelopParams negativeClarity;
    negativeClarity.clarity = -1.0F;

    const DevelopImageResult positiveResult = applyDevelop(source, positiveClarity);
    const DevelopImageResult negativeResult = applyDevelop(source, negativeClarity);

    ASSERT_TRUE(positiveResult.hasValue());
    ASSERT_TRUE(negativeResult.hasValue());
    EXPECT_GT(positiveResult.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_LT(negativeResult.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_EQ(90, positiveResult.value().pixelColor(1, 1).alpha());
}

TEST(Develop, PreservesClarityBytesForClampedBordersAndAlpha)
{
    for (const QSize& size : {QSize(1, 1), QSize(1, 9), QSize(9, 1), QSize(2, 2), QSize(9, 7), QSize(256, 3)})
    {
        QImage source(size, QImage::Format_RGBA8888);
        for (int row = 0; row < size.height(); ++row)
        {
            for (int column = 0; column < size.width(); ++column)
            {
                source.setPixelColor(column,
                                     row,
                                     QColor((row * 37 + column * 53 + 7) % 256,
                                            (row * 97 + column * 11 + 109) % 256,
                                            (row * 13 + column * 83 + 211) % 256,
                                            (row * 23 + column * 19 + 41) % 256));
            }
        }

        types::DevelopParams params;
        params.exposureEv = 0.35F;
        const DevelopImageResult base = applyDevelop(source, params);
        ASSERT_TRUE(base.hasValue());
        for (const float clarity : {-0.7F, 0.6F})
        {
            params.clarity = clarity;
            const DevelopImageResult actual = applyDevelop(source, params);
            ASSERT_TRUE(actual.hasValue());
            const QImage expected = referenceClarityImage(base.value(), clarity);
            for (int row = 0; row < size.height(); ++row)
            {
                const uchar* expectedPixels = expected.constScanLine(row);
                const uchar* actualPixels = actual.value().constScanLine(row);
                for (int byte = 0; byte < size.width() * 4; ++byte)
                {
                    EXPECT_EQ(expectedPixels[byte], actualPixels[byte])
                        << "size=" << size.width() << "x" << size.height() << " row=" << row << " byte=" << byte;
                }
            }
        }
    }
}

TEST(Develop, IncreasesContrastAndChromaForPositiveDehaze)
{
    const QImage source = createPixelImage(QColor{160, 128, 96, 90});
    types::DevelopParams params;
    params.dehaze = 1.0F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    const QColor adjusted = result.value().pixelColor(0, 0);
    EXPECT_GT(adjusted.red() - adjusted.blue(), source.pixelColor(0, 0).red() - source.pixelColor(0, 0).blue());
    EXPECT_GT(adjusted.red(), source.pixelColor(0, 0).red());
    EXPECT_EQ(90, adjusted.alpha());
}

TEST(Develop, AdjustsSelectedToneCurveRegions)
{
    const QImage shadows = createPixelImage(QColor{32, 32, 32, 90});
    const QImage highlights = createPixelImage(QColor{224, 224, 224, 90});
    types::DevelopParams shadowCurve;
    shadowCurve.toneCurveShadows = 1.0F;
    types::DevelopParams highlightCurve;
    highlightCurve.toneCurveHighlights = -1.0F;

    const DevelopImageResult shadowResult = applyDevelop(shadows, shadowCurve);
    const DevelopImageResult highlightResult = applyDevelop(highlights, highlightCurve);

    ASSERT_TRUE(shadowResult.hasValue());
    ASSERT_TRUE(highlightResult.hasValue());
    EXPECT_GT(shadowResult.value().pixelColor(0, 0).red(), shadows.pixelColor(0, 0).red());
    EXPECT_LT(highlightResult.value().pixelColor(0, 0).red(), highlights.pixelColor(0, 0).red());
    EXPECT_EQ(90, shadowResult.value().pixelColor(0, 0).alpha());
}

TEST(Develop, InterpolatesPointCurveAnchors)
{
    const QImage source = createPixelImage(QColor{188, 188, 188, 90});
    types::DevelopParams params;
    params.pointCurveMidtones = 1.0F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    EXPECT_GT(result.value().pixelColor(0, 0).red(), source.pixelColor(0, 0).red());
    EXPECT_EQ(90, result.value().pixelColor(0, 0).alpha());
}

TEST(Develop, ReducesLuminanceAndColorNoiseIndependently)
{
    QImage luminanceSource(QSize(3, 3), QImage::Format_RGBA8888);
    luminanceSource.fill(QColor(128, 128, 128, 90));
    luminanceSource.setPixelColor(1, 1, QColor(160, 160, 160, 90));
    types::DevelopParams luminanceParams;
    luminanceParams.luminanceNoiseReduction = 1.0F;

    QImage colorSource(QSize(3, 3), QImage::Format_RGBA8888);
    colorSource.fill(QColor(128, 128, 128, 90));
    colorSource.setPixelColor(1, 1, QColor(160, 128, 96, 90));
    types::DevelopParams colorParams;
    colorParams.colorNoiseReduction = 1.0F;

    const DevelopImageResult luminanceResult = applyDevelop(luminanceSource, luminanceParams);
    const DevelopImageResult colorResult = applyDevelop(colorSource, colorParams);

    ASSERT_TRUE(luminanceResult.hasValue());
    ASSERT_TRUE(colorResult.hasValue());
    EXPECT_LT(luminanceResult.value().pixelColor(1, 1).red(), luminanceSource.pixelColor(1, 1).red());
    EXPECT_LT(colorResult.value().pixelColor(1, 1).red() - colorResult.value().pixelColor(1, 1).blue(),
              colorSource.pixelColor(1, 1).red() - colorSource.pixelColor(1, 1).blue());
    EXPECT_EQ(90, colorResult.value().pixelColor(1, 1).alpha());
}

TEST(Develop, PreservesNoiseReductionBytesForClampedBordersAndAlpha)
{
    for (const QSize& size : {QSize(1, 1), QSize(1, 9), QSize(9, 1), QSize(2, 2), QSize(9, 7), QSize(256, 3)})
    {
        QImage source(size, QImage::Format_RGBA8888);
        for (int row = 0; row < size.height(); ++row)
        {
            for (int column = 0; column < size.width(); ++column)
            {
                source.setPixelColor(column,
                                     row,
                                     QColor((row * 37 + column * 53 + 7) % 256,
                                            (row * 97 + column * 11 + 109) % 256,
                                            (row * 13 + column * 83 + 211) % 256,
                                            (row * 23 + column * 19 + 41) % 256));
            }
        }

        types::DevelopParams params;
        params.exposureEv = 0.35F;
        const DevelopImageResult base = applyDevelop(source, params);
        ASSERT_TRUE(base.hasValue());
        for (const std::pair<float, float> strengths :
             {std::pair{0.7F, 0.0F}, std::pair{0.0F, 0.6F}, std::pair{0.7F, 0.6F}})
        {
            params.luminanceNoiseReduction = strengths.first;
            params.colorNoiseReduction = strengths.second;
            const DevelopImageResult actual = applyDevelop(source, params);
            ASSERT_TRUE(actual.hasValue());
            const QImage expected = referenceNoiseReductionImage(base.value(), strengths.first, strengths.second);
            for (int row = 0; row < size.height(); ++row)
            {
                const uchar* expectedPixels = expected.constScanLine(row);
                const uchar* actualPixels = actual.value().constScanLine(row);
                for (int byte = 0; byte < size.width() * 4; ++byte)
                {
                    EXPECT_EQ(expectedPixels[byte], actualPixels[byte])
                        << "size=" << size.width() << "x" << size.height() << " row=" << row << " byte=" << byte;
                }
            }
        }
    }
}

TEST(Develop, PreservesNoiseReductionThenClarityStageOrderAndBytes)
{
    for (const QSize& size : {QSize(1, 1), QSize(1, 7), QSize(7, 1), QSize(2, 2), QSize(9, 7)})
    {
        QImage source(size, QImage::Format_RGBA8888);
        for (int row = 0; row < size.height(); ++row)
        {
            for (int column = 0; column < size.width(); ++column)
            {
                source.setPixelColor(column,
                                     row,
                                     QColor((row * 37 + column * 53 + 7) % 256,
                                            (row * 97 + column * 11 + 109) % 256,
                                            (row * 13 + column * 83 + 211) % 256,
                                            (row * 23 + column * 19 + 41) % 256));
            }
        }

        types::DevelopParams params;
        params.exposureEv = 0.35F;
        const DevelopImageResult base = applyDevelop(source, params);
        ASSERT_TRUE(base.hasValue());

        params.luminanceNoiseReduction = 0.7F;
        params.colorNoiseReduction = 0.6F;
        params.clarity = 0.8F;
        const DevelopImageResult actual = applyDevelop(source, params);
        ASSERT_TRUE(actual.hasValue());

        const QImage expected = referenceFloat32NoiseReductionThenClarityImage(
            base.value(), params.luminanceNoiseReduction, params.colorNoiseReduction, params.clarity);
        for (int row = 0; row < size.height(); ++row)
        {
            const uchar* expectedPixels = expected.constScanLine(row);
            const uchar* actualPixels = actual.value().constScanLine(row);
            for (int byte = 0; byte < size.width() * 4; ++byte)
            {
                EXPECT_EQ(expectedPixels[byte], actualPixels[byte])
                    << "size=" << size.width() << "x" << size.height() << " row=" << row << " byte=" << byte;
            }
        }

        if (size == QSize(9, 7))
        {
            const QImage reversed = referenceNoiseReductionImage(referenceClarityImage(base.value(), params.clarity),
                                                                 params.luminanceNoiseReduction,
                                                                 params.colorNoiseReduction);
            EXPECT_NE(expected, reversed);
        }
    }
}

TEST(Develop, PreservesBoundedFloat32SpatialChainAcrossPermitsAndDynamicHalo)
{
    for (const QSize& size : {QSize(17, 131), QSize(5, 513)})
    {
        QImage source(size, QImage::Format_RGBA8888);
        for (int row = 0; row < size.height(); ++row)
        {
            for (int column = 0; column < size.width(); ++column)
            {
                source.setPixelColor(column,
                                     row,
                                     QColor((row * 37 + column * 53 + 7) % 256,
                                            (row * 97 + column * 11 + 109) % 256,
                                            (row * 13 + column * 83 + 211) % 256,
                                            (row * 23 + column * 19 + 41) % 256));
            }
        }

        types::DevelopParams params;
        params.exposureEv = 0.35F;
        params.luminanceNoiseReduction = 0.7F;
        params.colorNoiseReduction = 0.6F;
        params.clarity = 0.8F;
        params.sharpeningAmount = 0.65F;
        params.sharpeningDetail = 0.35F;
        params.sharpeningMasking = 0.4F;
        types::DevelopParams baseParams;
        baseParams.exposureEv = params.exposureEv;
        const DevelopImageResult base = applyDevelop(source, baseParams);
        ASSERT_TRUE(base.hasValue());

        for (const float radius : {1.0F, 2.0F, 3.0F})
        {
            params.sharpeningRadius = radius;
            const DevelopImageResult serial = applyDevelop(source, params, 1);
            const DevelopImageResult fourPermits = applyDevelop(source, params, 4);
            const DevelopImageResult eightPermits = applyDevelop(source, params, 8);
            const QImage expected = referenceFloat32SpatialChainImage(base.value(), params);

            ASSERT_TRUE(serial.hasValue());
            ASSERT_TRUE(fourPermits.hasValue());
            ASSERT_TRUE(eightPermits.hasValue());
            EXPECT_EQ(expected, serial.value())
                << "size=" << size.width() << "x" << size.height() << " radius=" << radius;
            EXPECT_EQ(serial.value(), fourPermits.value())
                << "size=" << size.width() << "x" << size.height() << " radius=" << radius;
            EXPECT_EQ(serial.value(), eightPermits.value())
                << "size=" << size.width() << "x" << size.height() << " radius=" << radius;
        }
    }
}

TEST(Develop, PreservesOptionalFloat32SpatialStagesAcrossPermitCounts)
{
    const QSize size{13, 131};
    QImage source(size, QImage::Format_RGBA8888);
    for (int row = 0; row < size.height(); ++row)
    {
        for (int column = 0; column < size.width(); ++column)
        {
            source.setPixelColor(column,
                                 row,
                                 QColor((row * 43 + column * 59 + 17) % 256,
                                        (row * 89 + column * 23 + 71) % 256,
                                        (row * 31 + column * 97 + 149) % 256,
                                        (row * 19 + column * 29 + 37) % 256));
        }
    }

    struct SpatialCase
    {
        const char* name;
        types::DevelopParams params;
    };
    std::array<SpatialCase, 5> cases{};
    cases[0].name = "noise-only";
    cases[0].params.luminanceNoiseReduction = 0.7F;
    cases[0].params.colorNoiseReduction = 0.6F;
    cases[1].name = "clarity-only";
    cases[1].params.clarity = 0.8F;
    cases[2].name = "sharpening-only";
    cases[2].params.sharpeningAmount = 0.65F;
    cases[2].params.sharpeningRadius = 3.0F;
    cases[2].params.sharpeningDetail = 0.35F;
    cases[2].params.sharpeningMasking = 0.4F;
    cases[3] = cases[2];
    cases[3].name = "noise-sharpening";
    cases[3].params.luminanceNoiseReduction = 0.7F;
    cases[3].params.colorNoiseReduction = 0.6F;
    cases[3].params.sharpeningRadius = 2.0F;
    cases[4] = cases[2];
    cases[4].name = "clarity-sharpening";
    cases[4].params.clarity = 0.8F;

    for (const SpatialCase& testCase : cases)
    {
        const DevelopImageResult serial = applyDevelop(source, testCase.params, 1);
        const DevelopImageResult parallel = applyDevelop(source, testCase.params, 8);
        const QImage expected = referenceFloat32SpatialChainImage(source, testCase.params);

        ASSERT_TRUE(serial.hasValue()) << testCase.name;
        ASSERT_TRUE(parallel.hasValue()) << testCase.name;
        EXPECT_EQ(expected, serial.value()) << testCase.name;
        EXPECT_EQ(serial.value(), parallel.value()) << testCase.name;
    }
}

TEST(Develop, AdjustsLocalDetailForSharpeningAmount)
{
    QImage source(QSize(3, 3), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(1, 1, QColor(160, 160, 160, 90));
    types::DevelopParams sharpen;
    sharpen.sharpeningAmount = 1.0F;
    types::DevelopParams soften;
    soften.sharpeningAmount = -1.0F;

    const DevelopImageResult sharpened = applyDevelop(source, sharpen);
    const DevelopImageResult softened = applyDevelop(source, soften);

    ASSERT_TRUE(sharpened.hasValue());
    ASSERT_TRUE(softened.hasValue());
    EXPECT_GT(sharpened.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_LT(softened.value().pixelColor(1, 1).red(), source.pixelColor(1, 1).red());
    EXPECT_EQ(90, sharpened.value().pixelColor(1, 1).alpha());
}

TEST(Develop, PreservesSharpeningEdgeClampingAndAlpha)
{
    QImage source(QSize(2, 2), QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(100, 100, 100, 90));
    source.setPixelColor(1, 0, QColor(120, 120, 120, 90));
    source.setPixelColor(0, 1, QColor(140, 140, 140, 90));
    source.setPixelColor(1, 1, QColor(160, 160, 160, 90));
    types::DevelopParams params;
    params.sharpeningAmount = 1.0F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QColor(80, 80, 80, 90), result.value().pixelColor(0, 0));
    EXPECT_EQ(QColor(180, 180, 180, 90), result.value().pixelColor(1, 1));
}

TEST(Develop, AdjustsSharpeningScaleAndDetailWithRadius)
{
    QImage source(QSize(7, 7), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(3, 3, QColor(160, 160, 160, 90));
    types::DevelopParams broadDetail;
    broadDetail.sharpeningAmount = 1.0F;
    broadDetail.sharpeningRadius = 3.0F;
    broadDetail.sharpeningDetail = -1.0F;
    types::DevelopParams fineDetail = broadDetail;
    fineDetail.sharpeningDetail = 1.0F;

    const DevelopImageResult broadResult = applyDevelop(source, broadDetail);
    const DevelopImageResult fineResult = applyDevelop(source, fineDetail);

    ASSERT_TRUE(broadResult.hasValue());
    ASSERT_TRUE(fineResult.hasValue());
    EXPECT_GT(broadResult.value().pixelColor(3, 3).red(), source.pixelColor(3, 3).red());
    EXPECT_NE(broadResult.value().pixelColor(3, 3).red(), fineResult.value().pixelColor(3, 3).red());
    EXPECT_EQ(90, broadResult.value().pixelColor(3, 3).alpha());
}

TEST(Develop, RoundsControlledSharpeningHalfSteps)
{
    QImage raised(QSize(5, 5), QImage::Format_RGBA8888);
    raised.fill(QColor(127, 127, 127, 90));
    raised.setPixelColor(2, 2, QColor(128, 128, 128, 90));
    raised.setPixelColor(0, 0, QColor(126, 126, 126, 90));
    QImage lowered(QSize(5, 5), QImage::Format_RGBA8888);
    lowered.fill(QColor(129, 129, 129, 90));
    lowered.setPixelColor(2, 2, QColor(128, 128, 128, 90));
    lowered.setPixelColor(0, 0, QColor(130, 130, 130, 90));
    types::DevelopParams params;
    params.sharpeningAmount = 0.5F;
    params.sharpeningRadius = 2.0F;
    params.sharpeningDetail = -1.0F;

    const DevelopImageResult raisedResult = applyDevelop(raised, params);
    const DevelopImageResult loweredResult = applyDevelop(lowered, params);

    ASSERT_TRUE(raisedResult.hasValue());
    ASSERT_TRUE(loweredResult.hasValue());
    EXPECT_EQ(129, raisedResult.value().pixelColor(2, 2).red());
    EXPECT_EQ(128, loweredResult.value().pixelColor(2, 2).red());
    EXPECT_EQ(90, raisedResult.value().pixelColor(2, 2).alpha());
}

TEST(Develop, MasksSharpeningAwayFromLowContrastPixels)
{
    QImage source(QSize(5, 5), QImage::Format_RGBA8888);
    source.fill(QColor(128, 128, 128, 90));
    source.setPixelColor(2, 2, QColor(160, 160, 160, 90));
    types::DevelopParams unmasked;
    unmasked.sharpeningAmount = 1.0F;
    types::DevelopParams masked = unmasked;
    masked.sharpeningMasking = 1.0F;

    const DevelopImageResult unmaskedResult = applyDevelop(source, unmasked);
    const DevelopImageResult maskedResult = applyDevelop(source, masked);

    ASSERT_TRUE(unmaskedResult.hasValue());
    ASSERT_TRUE(maskedResult.hasValue());
    EXPECT_GT(unmaskedResult.value().pixelColor(2, 2).red(), maskedResult.value().pixelColor(2, 2).red());
    EXPECT_GT(maskedResult.value().pixelColor(2, 2).red(), source.pixelColor(2, 2).red());
    EXPECT_EQ(90, maskedResult.value().pixelColor(2, 2).alpha());
}

TEST(Develop, RadiusOneMaskedSharpeningKeepsDetailEquivalent)
{
    QImage source(QSize(3, 3), QImage::Format_RGBA8888);
    source.fill(QColor(128, 120, 112, 90));
    source.setPixelColor(0, 0, QColor(255, 0, 64, 70));
    source.setPixelColor(1, 1, QColor(80, 200, 140, 110));

    for (const float amount : {1.0F, -1.0F})
    {
        for (const float masking : {0.1F, 1.0F})
        {
            types::DevelopParams broadDetail;
            broadDetail.sharpeningAmount = amount;
            broadDetail.sharpeningRadius = 1.0F;
            broadDetail.sharpeningDetail = -1.0F;
            broadDetail.sharpeningMasking = masking;
            types::DevelopParams fineDetail = broadDetail;
            fineDetail.sharpeningDetail = 1.0F;

            const DevelopImageResult broadResult = applyDevelop(source, broadDetail);
            const DevelopImageResult fineResult = applyDevelop(source, fineDetail);

            ASSERT_TRUE(broadResult.hasValue());
            ASSERT_TRUE(fineResult.hasValue());
            EXPECT_EQ(broadResult.value(), fineResult.value());
            EXPECT_EQ(source.pixelColor(0, 0).alpha(), broadResult.value().pixelColor(0, 0).alpha());
            EXPECT_EQ(source.pixelColor(1, 1).alpha(), broadResult.value().pixelColor(1, 1).alpha());
        }
    }
}

TEST(Develop, PreservesControlledSharpeningBytesAtClampedBorders)
{
    for (const QSize& size : {QSize(1, 1), QSize(1, 7), QSize(7, 1), QSize(2, 2), QSize(9, 7)})
    {
        QImage source(size, QImage::Format_RGBA8888);
        for (int row = 0; row < size.height(); ++row)
        {
            for (int column = 0; column < size.width(); ++column)
            {
                source.setPixelColor(column,
                                     row,
                                     QColor((row * 37 + column * 53 + 7) % 256,
                                            (row * 97 + column * 11 + 109) % 256,
                                            (row * 13 + column * 83 + 211) % 256,
                                            (row * 23 + column * 19 + 41) % 256));
            }
        }

        types::DevelopParams params;
        params.exposureEv = 0.35F;
        const DevelopImageResult base = applyDevelop(source, params);
        ASSERT_TRUE(base.hasValue());
        for (const float amount : {-0.65F, 0.8F})
        {
            for (const float radius : {1.0F, 2.0F, 3.0F})
            {
                for (const float masking : {0.0F, 0.6F})
                {
                    if (radius == 1.0F && masking == 0.0F)
                    {
                        continue;  // 기본 Sharpening 경로는 별도 test에서 확인합니다.
                    }
                    for (const float detail : {-1.0F, 1.0F})
                    {
                        params.sharpeningAmount = amount;
                        params.sharpeningRadius = radius;
                        params.sharpeningDetail = detail;
                        params.sharpeningMasking = masking;
                        const DevelopImageResult actual = applyDevelop(source, params);
                        ASSERT_TRUE(actual.hasValue());
                        const QImage expected = referenceControlledSharpeningImage(base.value(), params);
                        for (int row = 0; row < size.height(); ++row)
                        {
                            const uchar* actualPixels = actual.value().constScanLine(row);
                            const uchar* expectedPixels = expected.constScanLine(row);
                            for (int byte = 0; byte < size.width() * 4; ++byte)
                            {
                                EXPECT_EQ(expectedPixels[byte], actualPixels[byte])
                                    << "size=" << size.width() << "x" << size.height() << " radius=" << radius
                                    << " masking=" << masking << " detail=" << detail << " row=" << row
                                    << " byte=" << byte;
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(Develop, RejectsNullImage)
{
    const DevelopImageResult result = applyDevelop(QImage{}, types::DevelopParams{});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsNonPositiveCpuPermitCount)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});

    const DevelopImageResult result = applyDevelop(source, types::DevelopParams{}, 0);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, ReportsMaximumUsefulCpuPermitsForActiveNeighborhood)
{
    types::DevelopParams params;
    EXPECT_EQ(1, maximumUsefulDevelopCpuPermits(513, params));

    params.clarity = 0.8F;
    EXPECT_EQ(1, maximumUsefulDevelopCpuPermits(1, params));
    EXPECT_GE(maximumUsefulDevelopCpuPermits(513, params), 4);

    params.clarity = 0.0F;
    params.sharpeningAmount = 0.5F;
    EXPECT_GE(maximumUsefulDevelopCpuPermits(513, params), 4);
}

TEST(Develop, RejectsNonFiniteExposure)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult result =
        applyDevelop(source, types::DevelopParams{std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidContrast)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult outOfRange = applyDevelop(source, types::DevelopParams{0.0F, 1.1F});
    const DevelopImageResult nonFinite =
        applyDevelop(source, types::DevelopParams{0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(outOfRange.hasError());
    ASSERT_TRUE(nonFinite.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, outOfRange.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, nonFinite.error().code);
    EXPECT_EQ(QStringLiteral("Develop contrast must be finite and between -1.0 and 1.0."), outOfRange.error().message);
    EXPECT_EQ(QStringLiteral("Develop contrast must be finite and between -1.0 and 1.0."), nonFinite.error().message);
}

TEST(Develop, RejectsInvalidHighlightAndShadow)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult invalidHighlights = applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 1.1F, 0.0F});
    const DevelopImageResult invalidShadows =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(invalidHighlights.hasError());
    ASSERT_TRUE(invalidShadows.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidHighlights.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidShadows.error().code);
}

TEST(Develop, RejectsInvalidWhiteAndBlack)
{
    const QImage source = createPixelImage(QColor{128, 128, 128, 255});
    const DevelopImageResult invalidWhites =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 1.1F, 0.0F});
    const DevelopImageResult invalidBlacks = applyDevelop(
        source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(invalidWhites.hasError());
    ASSERT_TRUE(invalidBlacks.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidWhites.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidBlacks.error().code);
}

TEST(Develop, RejectsInvalidSaturation)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    const DevelopImageResult outOfRange =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.1F});
    const DevelopImageResult nonFinite = applyDevelop(
        source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(outOfRange.hasError());
    ASSERT_TRUE(nonFinite.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, outOfRange.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, nonFinite.error().code);
}

TEST(Develop, RejectsInvalidVibrance)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    const DevelopImageResult outOfRange =
        applyDevelop(source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.1F});
    const DevelopImageResult nonFinite = applyDevelop(
        source, types::DevelopParams{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, std::numeric_limits<float>::infinity()});

    ASSERT_TRUE(outOfRange.hasError());
    ASSERT_TRUE(nonFinite.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, outOfRange.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, nonFinite.error().code);
}

TEST(Develop, RejectsInvalidWhiteBalanceValues)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams invalidTemperature;
    invalidTemperature.whiteBalanceTemperatureKelvin = 1999.0F;
    types::DevelopParams invalidTint;
    invalidTint.whiteBalanceTint = 1.1F;

    const DevelopImageResult temperatureResult = applyDevelop(source, invalidTemperature);
    const DevelopImageResult tintResult = applyDevelop(source, invalidTint);

    ASSERT_TRUE(temperatureResult.hasError());
    ASSERT_TRUE(tintResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, temperatureResult.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, tintResult.error().code);
}

TEST(Develop, RejectsInvalidClarity)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.clarity = 1.1F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidDehaze)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.dehaze = std::numeric_limits<float>::infinity();

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidSharpeningRadiusAndDetail)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams invalidRadius;
    invalidRadius.sharpeningRadius = 3.1F;
    types::DevelopParams invalidDetail;
    invalidDetail.sharpeningDetail = std::numeric_limits<float>::infinity();

    const DevelopImageResult radiusResult = applyDevelop(source, invalidRadius);
    const DevelopImageResult detailResult = applyDevelop(source, invalidDetail);

    ASSERT_TRUE(radiusResult.hasError());
    ASSERT_TRUE(detailResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, radiusResult.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, detailResult.error().code);
}

TEST(Develop, RejectsInvalidSharpeningMasking)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.sharpeningMasking = -0.1F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidNoiseReduction)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams luminanceParams;
    luminanceParams.luminanceNoiseReduction = 1.1F;
    types::DevelopParams colorParams;
    colorParams.colorNoiseReduction = std::numeric_limits<float>::infinity();

    const DevelopImageResult luminanceResult = applyDevelop(source, luminanceParams);
    const DevelopImageResult colorResult = applyDevelop(source, colorParams);

    ASSERT_TRUE(luminanceResult.hasError());
    ASSERT_TRUE(colorResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, luminanceResult.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, colorResult.error().code);
}

TEST(Develop, RejectsInvalidToneCurve)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.toneCurveLights = 1.1F;

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST(Develop, RejectsInvalidPointCurve)
{
    const QImage source = createPixelImage(QColor{128, 64, 32, 255});
    types::DevelopParams params;
    params.pointCurveWhite = std::numeric_limits<float>::infinity();

    const DevelopImageResult result = applyDevelop(source, params);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

}  // namespace
}  // namespace flexraw::core::develop

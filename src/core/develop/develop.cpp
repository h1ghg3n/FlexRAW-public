#include "develop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

#include <QImage>

namespace flexraw::core::develop
{
namespace
{

constexpr float MinimumContrast = -1.0F;
constexpr float MaximumContrast = 1.0F;
constexpr float ContrastPivot = 0.18F;
constexpr float BlackPivot = 0.25F;
constexpr float WhitePivot = 0.75F;
constexpr float RedLuminanceWeight = 0.2126F;
constexpr float GreenLuminanceWeight = 0.7152F;
constexpr float BlueLuminanceWeight = 0.0722F;
constexpr float MinimumWhiteBalanceTemperatureKelvin = 2000.0F;
constexpr float MaximumWhiteBalanceTemperatureKelvin = 50000.0F;
constexpr float MinimumWhiteBalanceTint = -1.0F;
constexpr float MaximumWhiteBalanceTint = 1.0F;
constexpr float ReferenceWhiteBalanceTemperatureKelvin = 6500.0F;
constexpr float MaximumWhiteBalanceTintScale = 0.25F;
constexpr float MinimumClarity = -1.0F;
constexpr float MaximumClarity = 1.0F;
constexpr float MinimumDehaze = -1.0F;
constexpr float MaximumDehaze = 1.0F;
constexpr float DehazeContrastScale = 0.5F;
constexpr float DehazeChromaScale = 0.25F;
constexpr float MinimumNoiseReduction = 0.0F;
constexpr float MaximumNoiseReduction = 1.0F;
constexpr float MinimumToneCurve = -1.0F;
constexpr float MaximumToneCurve = 1.0F;
constexpr float ToneCurveMaximumOffset = 0.25F;
constexpr float MinimumLuminanceForScaling = 0.00001F;
constexpr std::size_t SrgbChannelValueCount = 256;

struct LinearRgb
{
    float red;
    float green;
    float blue;
};

struct WhiteBalanceGains
{
    float red{1.0F};
    float green{1.0F};
    float blue{1.0F};
};

// 목적: develop 변환 실패를 설명하는 CoreError 값 생성
// 입력: message: 오류 설명
// 출력: InvalidArgument으로 분류된 CoreError 값
[[nodiscard]] types::CoreError makeInvalidArgumentError(QString message)
{
    return types::CoreError{
        types::ErrorCode::InvalidArgument,
        std::move(message),
    };
}

// 목적: 8-bit sRGB channel 전체에 대한 linear-light lookup table 생성
// 입력: 없음
// 출력: channel 값을 index로 사용하는 256-entry linear-light table
[[nodiscard]] std::array<float, SrgbChannelValueCount> buildSrgbToLinearLookup()
{
    std::array<float, SrgbChannelValueCount> lookup{};
    for (std::size_t channel = 0; channel < lookup.size(); ++channel)
    {
        const float encodedValue = static_cast<float>(channel) / 255.0F;
        lookup[channel] =
            encodedValue <= 0.04045F ? encodedValue / 12.92F : std::pow((encodedValue + 0.055F) / 1.055F, 2.4F);
    }
    return lookup;
}

const std::array<float, SrgbChannelValueCount> SrgbToLinearLookup = buildSrgbToLinearLookup();

// 목적: 8-bit sRGB channel을 precomputed linear-light 값으로 변환
// 입력: channel: 0~255 범위의 sRGB channel 값
// 출력: lookup table의 0~1 linear-light 값
[[nodiscard]] float srgbChannelToLinear(uchar channel)
{
    return SrgbToLinearLookup[static_cast<std::size_t>(channel)];
}

// 목적: linear-light channel 값을 sRGB encoded 값으로 변환
// 입력: linearValue: 0~1 범위의 linear-light channel 값
// 출력: 0~1 범위의 sRGB encoded channel 값
[[nodiscard]] float linearToSrgb(float linearValue)
{
    if (linearValue <= 0.0031308F)
    {
        return linearValue * 12.92F;
    }

    return 1.055F * std::pow(linearValue, 1.0F / 2.4F) - 0.055F;
}

// 목적: normalized channel 값을 8-bit sRGB channel 값으로 변환
// 입력: value: 변환할 normalized channel 값
// 출력: 0~255 범위의 channel 값
[[nodiscard]] uchar toChannelValue(float value)
{
    const float clampedValue = std::clamp(value, 0.0F, 1.0F);
    return static_cast<uchar>(std::lround(clampedValue * 255.0F));
}

// 목적: linear-light channel에 노출과 contrast를 순서대로 적용
// 입력: linearValue: 원본 linear-light channel 값, exposureScale: 노출 배율, contrast: contrast 값
// 출력: 현상된 linear-light channel 값
[[nodiscard]] float applyToneToLinearChannel(float linearValue, float exposureScale, float contrast)
{
    const float exposedValue = linearValue * exposureScale;
    const float contrastScale = 1.0F + contrast;
    return ContrastPivot + (exposedValue - ContrastPivot) * contrastScale;
}

// 목적: linear-light shadow 영역을 지정한 양만큼 올리거나 내림
// 입력: linearValue: 현재 linear-light channel 값, shadows: [-1, 1] shadow 조정값
// 출력: shadow 조정이 적용된 linear-light channel 값
[[nodiscard]] float applyShadowsToLinearChannel(float linearValue, float shadows)
{
    const float normalizedShadow = 1.0F - std::clamp(linearValue / ContrastPivot, 0.0F, 1.0F);
    const float mask = normalizedShadow * normalizedShadow;
    const float targetValue = shadows >= 0.0F ? ContrastPivot : 0.0F;
    return linearValue + (targetValue - linearValue) * std::abs(shadows) * mask;
}

// 목적: linear-light highlight 영역을 지정한 양만큼 올리거나 내림
// 입력: linearValue: 현재 linear-light channel 값, highlights: [-1, 1] highlight 조정값
// 출력: highlight 조정이 적용된 linear-light channel 값
[[nodiscard]] float applyHighlightsToLinearChannel(float linearValue, float highlights)
{
    const float normalizedHighlight = std::clamp((linearValue - ContrastPivot) / (1.0F - ContrastPivot), 0.0F, 1.0F);
    const float mask = normalizedHighlight * normalizedHighlight;
    const float targetValue = highlights >= 0.0F ? 1.0F : ContrastPivot;
    return linearValue + (targetValue - linearValue) * std::abs(highlights) * mask;
}

// 목적: 색온도를 근사 sRGB white point로 변환
// 입력: temperatureKelvin: 2000~50000 범위의 색온도
// 출력: 각 channel이 0~1인 sRGB white point
[[nodiscard]] LinearRgb colorTemperatureToSrgb(float temperatureKelvin)
{
    const float temperature = temperatureKelvin / 100.0F;
    const float red = temperature <= 66.0F
                          ? 1.0F
                          : std::clamp(1.292936186F * std::pow(temperature - 60.0F, -0.1332047592F), 0.0F, 1.0F);
    const float green = temperature <= 66.0F
                            ? std::clamp(0.3900815788F * std::log(temperature) - 0.6318414438F, 0.0F, 1.0F)
                            : std::clamp(1.129890861F * std::pow(temperature - 60.0F, -0.0755148492F), 0.0F, 1.0F);
    const float blue =
        temperature >= 66.0F
            ? 1.0F
            : (temperature <= 19.0F
                   ? 0.0F
                   : std::clamp(0.5432067891F * std::log(temperature - 10.0F) - 1.196254089F, 0.0F, 1.0F));
    return {red, green, blue};
}

// 목적: custom white balance 값에서 camera white balance 기준의 linear RGB gain 계산
// 입력: temperatureKelvin: 목표 색온도, tint: 녹색 channel 보정값
// 출력: 현상 전 linear-light pixel에 곱할 RGB gain
[[nodiscard]] WhiteBalanceGains calculateWhiteBalanceGains(float temperatureKelvin, float tint)
{
    const LinearRgb target = colorTemperatureToSrgb(temperatureKelvin);
    const LinearRgb reference = colorTemperatureToSrgb(ReferenceWhiteBalanceTemperatureKelvin);
    const float greenTintScale = 1.0F - tint * MaximumWhiteBalanceTintScale;
    return {
        target.red / reference.red,
        (target.green / reference.green) * greenTintScale,
        target.blue / reference.blue,
    };
}

// 목적: linear-light black endpoint를 지정한 양만큼 올리거나 내림
// 입력: linearValue: 현재 linear-light channel 값, blacks: [-1, 1] black 조정값
// 출력: black 조정이 적용된 linear-light channel 값
[[nodiscard]] float applyBlacksToLinearChannel(float linearValue, float blacks)
{
    const float normalizedBlack = 1.0F - std::clamp(linearValue / BlackPivot, 0.0F, 1.0F);
    const float mask = normalizedBlack * normalizedBlack;
    const float targetValue = blacks >= 0.0F ? BlackPivot : 0.0F;
    return linearValue + (targetValue - linearValue) * std::abs(blacks) * mask;
}

// 목적: linear-light white endpoint를 지정한 양만큼 올리거나 내림
// 입력: linearValue: 현재 linear-light channel 값, whites: [-1, 1] white 조정값
// 출력: white 조정이 적용된 linear-light channel 값
[[nodiscard]] float applyWhitesToLinearChannel(float linearValue, float whites)
{
    const float normalizedWhite = std::clamp((linearValue - WhitePivot) / (1.0F - WhitePivot), 0.0F, 1.0F);
    const float mask = normalizedWhite * normalizedWhite;
    const float targetValue = whites >= 0.0F ? 1.0F : WhitePivot;
    return linearValue + (targetValue - linearValue) * std::abs(whites) * mask;
}

// 목적: parametric tone curve의 네 tonal region mask를 계산
// 입력: linearValue: 조정할 linear-light channel 값, center/width: region 중심과 폭
// 출력: 0~1 범위의 해당 tonal region 영향도
[[nodiscard]] float calculateToneCurveMask(float linearValue, float center, float width)
{
    return std::clamp(1.0F - std::abs(linearValue - center) / width, 0.0F, 1.0F);
}

// 목적: four-region parametric tone curve를 linear-light channel에 적용
// 입력: linearValue: 현재 linear-light channel 값, params: tone curve parameter 값
// 출력: tone curve가 적용된 linear-light channel 값
[[nodiscard]] float applyToneCurveToLinearChannel(float linearValue, const types::DevelopParams& params)
{
    const float shadowsMask = calculateToneCurveMask(linearValue, 0.0F, 0.25F);
    const float darksMask = calculateToneCurveMask(linearValue, 0.25F, 0.25F);
    const float lightsMask = calculateToneCurveMask(linearValue, 0.50F, 0.25F);
    const float highlightsMask = calculateToneCurveMask(linearValue, 0.85F, 0.30F);
    const float adjustment = params.toneCurveShadows * shadowsMask + params.toneCurveDarks * darksMask +
                             params.toneCurveLights * lightsMask + params.toneCurveHighlights * highlightsMask;
    return linearValue + adjustment * ToneCurveMaximumOffset;
}

// 목적: five-anchor RGB composite point curve를 linear-light channel에 적용
// 입력: linearValue: 현재 linear-light channel 값, params: point curve parameter 값
// 출력: point curve가 적용된 linear-light channel 값
[[nodiscard]] float applyPointCurveToLinearChannel(float linearValue, const types::DevelopParams& params)
{
    constexpr std::array<float, 5> AnchorOffsets{0.0F, 0.25F, 0.50F, 0.75F, 1.0F};
    const std::array<float, 5> pointOffsets{
        params.pointCurveBlack,
        params.pointCurveShadows,
        params.pointCurveMidtones,
        params.pointCurveHighlights,
        params.pointCurveWhite,
    };
    const float clampedValue = std::clamp(linearValue, 0.0F, 1.0F);
    const int lowerIndex = std::min(static_cast<int>(clampedValue / 0.25F), 3);
    const float interpolation = (clampedValue - AnchorOffsets[lowerIndex]) * 4.0F;
    const float offset =
        pointOffsets[lowerIndex] + (pointOffsets[lowerIndex + 1] - pointOffsets[lowerIndex]) * interpolation;
    return linearValue + offset * ToneCurveMaximumOffset;
}

// 목적: 한 sRGB channel에 light control을 적용해 linear-light 값으로 반환
// 입력: channel: 원본 8-bit sRGB channel 값, exposureScale: 노출 배율, params: tone parameter 값
// 출력: 현상된 linear-light channel 값
[[nodiscard]] float applyLightToLinearChannel(uchar channel,
                                              float whiteBalanceGain,
                                              float exposureScale,
                                              const types::DevelopParams& params)
{
    const float contrastAdjustedValue =
        applyToneToLinearChannel(srgbChannelToLinear(channel) * whiteBalanceGain, exposureScale, params.contrast);
    const float shadowAdjustedValue = applyShadowsToLinearChannel(contrastAdjustedValue, params.shadows);
    const float highlightAdjustedValue = applyHighlightsToLinearChannel(shadowAdjustedValue, params.highlights);
    const float blackAdjustedValue = applyBlacksToLinearChannel(highlightAdjustedValue, params.blacks);
    return applyPointCurveToLinearChannel(
        applyToneCurveToLinearChannel(applyWhitesToLinearChannel(blackAdjustedValue, params.whites), params), params);
}

// 목적: linear-light RGB pixel의 chroma를 Rec.709 luma 기준으로 일정 비율 조정
// 입력: pixel: 조정할 linear-light RGB 값, chromaScale: chroma에 곱할 0 이상 비율
// 출력: 없음
void scaleLinearPixelChroma(LinearRgb& pixel, float chromaScale)
{
    const float luminance =
        pixel.red * RedLuminanceWeight + pixel.green * GreenLuminanceWeight + pixel.blue * BlueLuminanceWeight;
    pixel.red = luminance + (pixel.red - luminance) * chromaScale;
    pixel.green = luminance + (pixel.green - luminance) * chromaScale;
    pixel.blue = luminance + (pixel.blue - luminance) * chromaScale;
}

// 목적: linear-light RGB pixel의 chroma를 Rec.709 luma 기준으로 saturation 조정
// 입력: pixel: 조정할 linear-light RGB 값, saturation: [-1, 1] saturation 조정값
// 출력: 없음
void applySaturationToLinearPixel(LinearRgb& pixel, float saturation)
{
    scaleLinearPixelChroma(pixel, 1.0F + saturation);
}

// 목적: 저채도 색을 우선해 linear-light RGB pixel의 chroma를 vibrance 조정
// 입력: pixel: 조정할 linear-light RGB 값, vibrance: [-1, 1] vibrance 조정값
// 출력: 없음
void applyVibranceToLinearPixel(LinearRgb& pixel, float vibrance)
{
    if (vibrance <= 0.0F)
    {
        scaleLinearPixelChroma(pixel, 1.0F + vibrance);
        return;
    }

    const float maximum = std::max({pixel.red, pixel.green, pixel.blue});
    const float minimum = std::min({pixel.red, pixel.green, pixel.blue});
    const float currentChroma = maximum <= 0.0F ? 0.0F : (maximum - minimum) / maximum;
    const float chromaScale = 1.0F + vibrance * (1.0F - currentChroma);
    scaleLinearPixelChroma(pixel, chromaScale);
}

// 목적: linear-light RGB pixel의 대비와 chroma를 함께 조정해 haze를 줄이거나 더함
// 입력: pixel: 조정할 linear-light RGB 값, dehaze: [-1, 1] dehaze 조정값
// 출력: 없음
void applyDehazeToLinearPixel(LinearRgb& pixel, float dehaze)
{
    if (dehaze == 0.0F)
    {
        return;
    }

    const float contrastScale = 1.0F + dehaze * DehazeContrastScale;
    pixel.red = ContrastPivot + (pixel.red - ContrastPivot) * contrastScale;
    pixel.green = ContrastPivot + (pixel.green - ContrastPivot) * contrastScale;
    pixel.blue = ContrastPivot + (pixel.blue - ContrastPivot) * contrastScale;
    scaleLinearPixelChroma(pixel, 1.0F + dehaze * DehazeChromaScale);
}

// 목적: sRGB encoded RGB channel에서 linear-light Rec.709 휘도 계산
// 입력: red/green/blue: 0~255 범위의 sRGB encoded channel 값
// 출력: 0~1 범위의 linear-light 휘도
[[nodiscard]] float calculateLinearLuminance(uchar red, uchar green, uchar blue)
{
    return srgbChannelToLinear(red) * RedLuminanceWeight + srgbChannelToLinear(green) * GreenLuminanceWeight +
           srgbChannelToLinear(blue) * BlueLuminanceWeight;
}

// 목적: 3x3 local luminance 대비를 midtone에 한정해 clarity로 조정
// 입력: image: 조정할 RGBA sRGB image, clarity: [-1, 1] local contrast 조정값
// 출력: 없음
void applyClarityToImage(QImage& image, float clarity)
{
    if (clarity == 0.0F)
    {
        return;
    }

    const QImage sourceImage = image.copy();

    for (int row = 0; row < image.height(); ++row)
    {
        uchar* destinationPixels = image.scanLine(row);

        for (int column = 0; column < image.width(); ++column)
        {
            const uchar* sourcePixel = sourceImage.constScanLine(row) + column * 4;
            const float luminance = calculateLinearLuminance(sourcePixel[0], sourcePixel[1], sourcePixel[2]);
            float localLuminance = 0.0F;
            int sampleCount = 0;

            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, image.height() - 1);
                const uchar* samplePixels = sourceImage.constScanLine(sampleRow);

                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, image.width() - 1);
                    const uchar* samplePixel = samplePixels + sampleColumn * 4;
                    localLuminance += calculateLinearLuminance(samplePixel[0], samplePixel[1], samplePixel[2]);
                    ++sampleCount;
                }
            }

            localLuminance /= static_cast<float>(sampleCount);
            const float midtoneWeight = 1.0F - std::abs(luminance * 2.0F - 1.0F);
            const float adjustedLuminance =
                std::clamp(luminance + (luminance - localLuminance) * clarity * midtoneWeight, 0.0F, 1.0F);
            const float luminanceScale = luminance > MinimumLuminanceForScaling ? adjustedLuminance / luminance : 1.0F;
            uchar* destinationPixel = destinationPixels + column * 4;
            destinationPixel[0] = toChannelValue(linearToSrgb(srgbChannelToLinear(sourcePixel[0]) * luminanceScale));
            destinationPixel[1] = toChannelValue(linearToSrgb(srgbChannelToLinear(sourcePixel[1]) * luminanceScale));
            destinationPixel[2] = toChannelValue(linearToSrgb(srgbChannelToLinear(sourcePixel[2]) * luminanceScale));
        }
    }
}

// 목적: local luminance와 chroma 평균을 이용해 sRGB image의 noise를 완화
// 입력: image: 조정할 RGBA sRGB image, luminanceStrength/colorStrength: [0, 1] noise reduction 강도
// 출력: 없음
void applyNoiseReductionToImage(QImage& image, float luminanceStrength, float colorStrength)
{
    if (luminanceStrength == 0.0F && colorStrength == 0.0F)
    {
        return;
    }

    const QImage sourceImage = image.copy();
    for (int row = 0; row < image.height(); ++row)
    {
        uchar* destinationPixels = image.scanLine(row);
        for (int column = 0; column < image.width(); ++column)
        {
            const uchar* sourcePixel = sourceImage.constScanLine(row) + column * 4;
            const LinearRgb sourceLinear{
                srgbChannelToLinear(sourcePixel[0]),
                srgbChannelToLinear(sourcePixel[1]),
                srgbChannelToLinear(sourcePixel[2]),
            };
            const float sourceLuminance = sourceLinear.red * RedLuminanceWeight +
                                          sourceLinear.green * GreenLuminanceWeight +
                                          sourceLinear.blue * BlueLuminanceWeight;
            LinearRgb localAverage{};
            float localLuminance = 0.0F;
            for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
            {
                const int sampleRow = std::clamp(row + rowOffset, 0, image.height() - 1);
                const uchar* samplePixels = sourceImage.constScanLine(sampleRow);
                for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                {
                    const int sampleColumn = std::clamp(column + columnOffset, 0, image.width() - 1);
                    const uchar* samplePixel = samplePixels + sampleColumn * 4;
                    const LinearRgb sampleLinear{
                        srgbChannelToLinear(samplePixel[0]),
                        srgbChannelToLinear(samplePixel[1]),
                        srgbChannelToLinear(samplePixel[2]),
                    };
                    localAverage.red += sampleLinear.red;
                    localAverage.green += sampleLinear.green;
                    localAverage.blue += sampleLinear.blue;
                    localLuminance += sampleLinear.red * RedLuminanceWeight +
                                      sampleLinear.green * GreenLuminanceWeight +
                                      sampleLinear.blue * BlueLuminanceWeight;
                }
            }

            localAverage.red /= 9.0F;
            localAverage.green /= 9.0F;
            localAverage.blue /= 9.0F;
            localLuminance /= 9.0F;
            const float targetLuminance = sourceLuminance + (localLuminance - sourceLuminance) * luminanceStrength;
            const float luminanceScale =
                sourceLuminance > MinimumLuminanceForScaling ? targetLuminance / sourceLuminance : 1.0F;
            LinearRgb destinationLinear{
                sourceLinear.red * luminanceScale,
                sourceLinear.green * luminanceScale,
                sourceLinear.blue * luminanceScale,
            };

            if (colorStrength != 0.0F)
            {
                destinationLinear.red +=
                    (localAverage.red - localLuminance - (sourceLinear.red - sourceLuminance)) * colorStrength;
                destinationLinear.green +=
                    (localAverage.green - localLuminance - (sourceLinear.green - sourceLuminance)) * colorStrength;
                destinationLinear.blue +=
                    (localAverage.blue - localLuminance - (sourceLinear.blue - sourceLuminance)) * colorStrength;
            }

            uchar* destinationPixel = destinationPixels + column * 4;
            destinationPixel[0] = toChannelValue(linearToSrgb(destinationLinear.red));
            destinationPixel[1] = toChannelValue(linearToSrgb(destinationLinear.green));
            destinationPixel[2] = toChannelValue(linearToSrgb(destinationLinear.blue));
        }
    }
}

// 목적: 3x3 local average와의 차이로 sRGB image의 local detail을 조정
// 입력: image: 조정할 RGBA sRGB image, amount: [-1, 1] detail 조정값
// 출력: 없음
void applySharpeningToImage(QImage& image, float amount)
{
    if (amount == 0.0F)
    {
        return;
    }

    const QImage sourceImage = image.copy();
    for (int row = 0; row < image.height(); ++row)
    {
        for (int column = 0; column < image.width(); ++column)
        {
            const uchar* sourcePixel = sourceImage.constScanLine(row) + column * 4;
            uchar* destinationPixel = image.scanLine(row) + column * 4;
            for (int channel = 0; channel < 3; ++channel)
            {
                int localSum = 0;
                for (int rowOffset = -1; rowOffset <= 1; ++rowOffset)
                {
                    const int sampleRow = std::clamp(row + rowOffset, 0, image.height() - 1);
                    for (int columnOffset = -1; columnOffset <= 1; ++columnOffset)
                    {
                        const int sampleColumn = std::clamp(column + columnOffset, 0, image.width() - 1);
                        localSum += sourceImage.constScanLine(sampleRow)[sampleColumn * 4 + channel];
                    }
                }
                const float localAverage = static_cast<float>(localSum) / 9.0F;
                destinationPixel[channel] = static_cast<uchar>(std::lround(std::clamp(
                    static_cast<float>(sourcePixel[channel]) + (sourcePixel[channel] - localAverage) * amount,
                    0.0F,
                    255.0F)));
            }
        }
    }
}

// 목적: 주변 밝기 차이로 sharpening이 적용될 edge weight 계산
// 입력: image: 원본 RGBA sRGB image, row/column: 대상 pixel 위치, masking: [0, 1] 보호 강도
// 출력: 0~1 범위의 sharpening edge weight
[[nodiscard]] float calculateSharpeningEdgeWeight(const QImage& image, int row, int column, float masking)
{
    if (masking == 0.0F)
    {
        return 1.0F;
    }

    const uchar* sourcePixel = image.constScanLine(row) + column * 4;
    const uchar* leftPixel = image.constScanLine(row) + std::max(column - 1, 0) * 4;
    const uchar* rightPixel = image.constScanLine(row) + std::min(column + 1, image.width() - 1) * 4;
    const uchar* topPixel = image.constScanLine(std::max(row - 1, 0)) + column * 4;
    const uchar* bottomPixel = image.constScanLine(std::min(row + 1, image.height() - 1)) + column * 4;
    float maximumDifference = 0.0F;
    for (int channel = 0; channel < 3; ++channel)
    {
        const float sourceValue = static_cast<float>(sourcePixel[channel]);
        maximumDifference = std::max({maximumDifference,
                                      std::abs(sourceValue - static_cast<float>(leftPixel[channel])),
                                      std::abs(sourceValue - static_cast<float>(rightPixel[channel])),
                                      std::abs(sourceValue - static_cast<float>(topPixel[channel])),
                                      std::abs(sourceValue - static_cast<float>(bottomPixel[channel]))});
    }

    const float edgeStrength = maximumDifference / 255.0F;
    return 1.0F - masking * (1.0F - edgeStrength);
}

// 목적: 가변 반경과 detail/masking 제어값으로 sRGB image의 sharpening을 적용
// 입력: image: 조정할 RGBA sRGB image, amount/radius/detail/masking: sharpening 제어값
// 출력: 없음
void applyControlledSharpeningToImage(QImage& image, float amount, float radius, float detail, float masking)
{
    if (amount == 0.0F)
    {
        return;
    }

    const int kernelRadius = static_cast<int>(std::lround(radius));
    if (kernelRadius == 1 && masking == 0.0F)
    {
        applySharpeningToImage(image, amount);
        return;
    }

    const float detailWeight = (detail + 1.0F) * 0.5F;
    const QImage sourceImage = image.copy();
    for (int row = 0; row < image.height(); ++row)
    {
        for (int column = 0; column < image.width(); ++column)
        {
            const uchar* sourcePixel = sourceImage.constScanLine(row) + column * 4;
            uchar* destinationPixel = image.scanLine(row) + column * 4;
            const float edgeWeight = calculateSharpeningEdgeWeight(sourceImage, row, column, masking);
            for (int channel = 0; channel < 3; ++channel)
            {
                int broadSum = 0;
                int fineSum = 0;
                for (int rowOffset = -kernelRadius; rowOffset <= kernelRadius; ++rowOffset)
                {
                    const int sampleRow = std::clamp(row + rowOffset, 0, image.height() - 1);
                    const uchar* samplePixels = sourceImage.constScanLine(sampleRow);
                    for (int columnOffset = -kernelRadius; columnOffset <= kernelRadius; ++columnOffset)
                    {
                        const int sampleColumn = std::clamp(column + columnOffset, 0, image.width() - 1);
                        broadSum += samplePixels[sampleColumn * 4 + channel];
                        if (std::abs(rowOffset) <= 1 && std::abs(columnOffset) <= 1)
                        {
                            fineSum += samplePixels[sampleColumn * 4 + channel];
                        }
                    }
                }

                const float broadAverage =
                    static_cast<float>(broadSum) / static_cast<float>((kernelRadius * 2 + 1) * (kernelRadius * 2 + 1));
                const float fineAverage = static_cast<float>(fineSum) / 9.0F;
                const float sourceValue = static_cast<float>(sourcePixel[channel]);
                const float broadSignal = sourceValue - broadAverage;
                const float fineSignal = sourceValue - fineAverage;
                const float signal = broadSignal + (fineSignal - broadSignal) * detailWeight;
                destinationPixel[channel] = static_cast<uchar>(
                    std::lround(std::clamp(sourceValue + signal * amount * edgeWeight, 0.0F, 255.0F)));
            }
        }
    }
}

}  // namespace

// 목적: develop parameter 전체가 Core 허용 범위에 있는지 검증
// 입력: params: 검증할 develop parameter 값
// 출력: 유효한 parameter 복사본 또는 구조화된 오류
DevelopParamsValidationResult validateDevelopParams(const types::DevelopParams& params)
{
    if (!std::isfinite(params.exposureEv))
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop exposure EV must be finite.")));
    }

    if (!std::isfinite(params.contrast) || params.contrast < MinimumContrast || params.contrast > MaximumContrast)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop contrast must be finite and between -1.0 and 1.0.")));
    }

    if (!std::isfinite(params.highlights) || params.highlights < MinimumContrast || params.highlights > MaximumContrast)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop highlights must be finite and between -1.0 and 1.0.")));
    }

    if (!std::isfinite(params.shadows) || params.shadows < MinimumContrast || params.shadows > MaximumContrast)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop shadows must be finite and between -1.0 and 1.0.")));
    }

    if (!std::isfinite(params.whites) || params.whites < MinimumContrast || params.whites > MaximumContrast)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop whites must be finite and between -1.0 and 1.0.")));
    }

    if (!std::isfinite(params.blacks) || params.blacks < MinimumContrast || params.blacks > MaximumContrast)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop blacks must be finite and between -1.0 and 1.0.")));
    }

    if (!std::isfinite(params.saturation) || params.saturation < MinimumContrast || params.saturation > MaximumContrast)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop saturation must be finite and between -1.0 and 1.0.")));
    }

    if (!std::isfinite(params.vibrance) || params.vibrance < MinimumContrast || params.vibrance > MaximumContrast)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop vibrance must be finite and between -1.0 and 1.0.")));
    }

    if (params.whiteBalanceMode != types::WhiteBalanceMode::AsShot &&
        params.whiteBalanceMode != types::WhiteBalanceMode::Custom)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop white balance mode is invalid.")));
    }

    if (!std::isfinite(params.whiteBalanceTemperatureKelvin) ||
        params.whiteBalanceTemperatureKelvin < MinimumWhiteBalanceTemperatureKelvin ||
        params.whiteBalanceTemperatureKelvin > MaximumWhiteBalanceTemperatureKelvin)
    {
        return DevelopParamsValidationResult::failure(makeInvalidArgumentError(
            QStringLiteral("Develop white balance temperature must be between 2000 and 50000 Kelvin.")));
    }

    if (!std::isfinite(params.whiteBalanceTint) || params.whiteBalanceTint < MinimumWhiteBalanceTint ||
        params.whiteBalanceTint > MaximumWhiteBalanceTint)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop white balance tint must be between -1.0 and 1.0.")));
    }

    if (!std::isfinite(params.clarity) || params.clarity < MinimumClarity || params.clarity > MaximumClarity)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop clarity must be finite and between -1.0 and 1.0.")));
    }
    if (!std::isfinite(params.dehaze) || params.dehaze < MinimumDehaze || params.dehaze > MaximumDehaze)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop dehaze must be finite and between -1.0 and 1.0.")));
    }
    if (!std::isfinite(params.sharpeningAmount) || params.sharpeningAmount < -1.0F || params.sharpeningAmount > 1.0F)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop sharpening amount is invalid.")));
    }
    if (!std::isfinite(params.sharpeningRadius) || params.sharpeningRadius < 1.0F || params.sharpeningRadius > 3.0F)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop sharpening radius must be between 1.0 and 3.0.")));
    }
    if (!std::isfinite(params.sharpeningDetail) || params.sharpeningDetail < -1.0F || params.sharpeningDetail > 1.0F)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop sharpening detail must be between -1.0 and 1.0.")));
    }
    if (!std::isfinite(params.sharpeningMasking) || params.sharpeningMasking < 0.0F || params.sharpeningMasking > 1.0F)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop sharpening masking must be between 0.0 and 1.0.")));
    }
    if (!std::isfinite(params.luminanceNoiseReduction) || params.luminanceNoiseReduction < MinimumNoiseReduction ||
        params.luminanceNoiseReduction > MaximumNoiseReduction || !std::isfinite(params.colorNoiseReduction) ||
        params.colorNoiseReduction < MinimumNoiseReduction || params.colorNoiseReduction > MaximumNoiseReduction)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop noise reduction values must be between 0.0 and 1.0.")));
    }
    if (!std::isfinite(params.toneCurveShadows) || !std::isfinite(params.toneCurveDarks) ||
        !std::isfinite(params.toneCurveLights) || !std::isfinite(params.toneCurveHighlights) ||
        params.toneCurveShadows < MinimumToneCurve || params.toneCurveShadows > MaximumToneCurve ||
        params.toneCurveDarks < MinimumToneCurve || params.toneCurveDarks > MaximumToneCurve ||
        params.toneCurveLights < MinimumToneCurve || params.toneCurveLights > MaximumToneCurve ||
        params.toneCurveHighlights < MinimumToneCurve || params.toneCurveHighlights > MaximumToneCurve)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop tone curve values must be between -1.0 and 1.0.")));
    }
    if (!std::isfinite(params.pointCurveBlack) || !std::isfinite(params.pointCurveShadows) ||
        !std::isfinite(params.pointCurveMidtones) || !std::isfinite(params.pointCurveHighlights) ||
        !std::isfinite(params.pointCurveWhite) || params.pointCurveBlack < MinimumToneCurve ||
        params.pointCurveBlack > MaximumToneCurve || params.pointCurveShadows < MinimumToneCurve ||
        params.pointCurveShadows > MaximumToneCurve || params.pointCurveMidtones < MinimumToneCurve ||
        params.pointCurveMidtones > MaximumToneCurve || params.pointCurveHighlights < MinimumToneCurve ||
        params.pointCurveHighlights > MaximumToneCurve || params.pointCurveWhite < MinimumToneCurve ||
        params.pointCurveWhite > MaximumToneCurve)
    {
        return DevelopParamsValidationResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop point curve values must be between -1.0 and 1.0.")));
    }

    return DevelopParamsValidationResult::success(params);
}

// 목적: sRGB preview image에 non-destructive develop parameter를 적용
// 입력: sourceImage: sRGB preview 원본, params: 적용할 develop parameter 값
// 출력: 현상된 sRGB QImage 또는 구조화된 오류
// 에러 처리: null image와 유효하지 않은 develop parameter는 Result, 메모리 부족은 예외
DevelopImageResult applyDevelop(const QImage& sourceImage, const types::DevelopParams& params)
{
    if (sourceImage.isNull())
    {
        return DevelopImageResult::failure(makeInvalidArgumentError(QStringLiteral("Develop source image is null.")));
    }

    const DevelopParamsValidationResult validation = validateDevelopParams(params);
    if (validation.hasError())
    {
        return DevelopImageResult::failure(validation.error());
    }

    if (params.exposureEv == 0.0F && params.contrast == 0.0F && params.highlights == 0.0F && params.shadows == 0.0F &&
        params.whites == 0.0F && params.blacks == 0.0F && params.saturation == 0.0F && params.vibrance == 0.0F &&
        params.whiteBalanceMode == types::WhiteBalanceMode::AsShot && params.clarity == 0.0F && params.dehaze == 0.0F &&
        params.sharpeningAmount == 0.0F && params.luminanceNoiseReduction == 0.0F &&
        params.colorNoiseReduction == 0.0F && params.toneCurveShadows == 0.0F && params.toneCurveDarks == 0.0F &&
        params.toneCurveLights == 0.0F && params.toneCurveHighlights == 0.0F && params.pointCurveBlack == 0.0F &&
        params.pointCurveShadows == 0.0F && params.pointCurveMidtones == 0.0F && params.pointCurveHighlights == 0.0F &&
        params.pointCurveWhite == 0.0F)
    {
        return DevelopImageResult::success(sourceImage);
    }

    QImage developedImage = sourceImage.convertToFormat(QImage::Format_RGBA8888);

    if (developedImage.isNull())
    {
        return DevelopImageResult::failure(
            makeInvalidArgumentError(QStringLiteral("Unable to convert develop source image.")));
    }

    const float exposureScale = std::exp2(params.exposureEv);
    const WhiteBalanceGains whiteBalanceGains =
        params.whiteBalanceMode == types::WhiteBalanceMode::Custom
            ? calculateWhiteBalanceGains(params.whiteBalanceTemperatureKelvin, params.whiteBalanceTint)
            : WhiteBalanceGains{};

    for (int row = 0; row < developedImage.height(); ++row)
    {
        uchar* pixels = developedImage.scanLine(row);

        for (int column = 0; column < developedImage.width(); ++column)
        {
            uchar* pixel = pixels + column * 4;
            LinearRgb linearPixel{
                applyLightToLinearChannel(pixel[0], whiteBalanceGains.red, exposureScale, params),
                applyLightToLinearChannel(pixel[1], whiteBalanceGains.green, exposureScale, params),
                applyLightToLinearChannel(pixel[2], whiteBalanceGains.blue, exposureScale, params),
            };
            applySaturationToLinearPixel(linearPixel, params.saturation);
            applyVibranceToLinearPixel(linearPixel, params.vibrance);
            applyDehazeToLinearPixel(linearPixel, params.dehaze);
            pixel[0] = toChannelValue(linearToSrgb(linearPixel.red));
            pixel[1] = toChannelValue(linearToSrgb(linearPixel.green));
            pixel[2] = toChannelValue(linearToSrgb(linearPixel.blue));
        }
    }

    applyNoiseReductionToImage(developedImage, params.luminanceNoiseReduction, params.colorNoiseReduction);
    applyClarityToImage(developedImage, params.clarity);
    applyControlledSharpeningToImage(developedImage,
                                     params.sharpeningAmount,
                                     params.sharpeningRadius,
                                     params.sharpeningDetail,
                                     params.sharpeningMasking);

    return DevelopImageResult::success(developedImage);
}

}  // namespace flexraw::core::develop

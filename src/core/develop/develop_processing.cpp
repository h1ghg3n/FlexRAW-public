#include "develop_processing.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "bounded_float32_neighborhood.h"
#include "develop_params_validation.h"
#include "develop_processing_detail.h"
#include "hsl_processing.h"

namespace flexraw::core::develop
{
namespace
{

constexpr float ContrastPivot = 0.18F;
constexpr float BlackPivot = 0.25F;
constexpr float WhitePivot = 0.75F;
constexpr float RedLuminanceWeight = 0.2126F;
constexpr float GreenLuminanceWeight = 0.7152F;
constexpr float BlueLuminanceWeight = 0.0722F;
constexpr float ReferenceWhiteBalanceTemperatureKelvin = 6500.0F;
constexpr float MaximumWhiteBalanceTintScale = 0.25F;
constexpr float DehazeContrastScale = 0.5F;
constexpr float DehazeChromaScale = 0.25F;
constexpr float ToneCurveMaximumOffset = 0.25F;
constexpr std::size_t Rgba8BytesPerPixel = 4;
constexpr std::size_t SrgbChannelValueCount = 256;
constexpr std::size_t LinearToSrgbTransitionCount = SrgbChannelValueCount - 1;
constexpr std::size_t LinearToSrgbLookupSize = 65536;
constexpr float LinearToSrgbLookupScale = static_cast<float>(LinearToSrgbLookupSize - 1);

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
[[nodiscard]] float srgbChannelToLinear(std::uint8_t channel)
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
[[nodiscard]] std::uint8_t toChannelValue(float value)
{
    const float clampedValue = std::clamp(value, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(clampedValue * 255.0F + 0.5F);
}

// 목적: 기존 linear-to-sRGB 식과 8-bit 반올림을 그대로 사용해 기준 channel 값을 계산
// 입력: linearValue — 변환할 linear-light channel 값
// 출력: 기존 scalar projection과 동일한 0~255 channel 값
[[nodiscard]] std::uint8_t quantizeLinearToSrgbReference(float linearValue)
{
    return toChannelValue(linearToSrgb(linearValue));
}

// 목적: 지정한 8-bit channel 값으로 처음 전이하는 양의 float 입력을 탐색
// 입력: channel — 전이 뒤의 1~255 sRGB channel 값
// 출력: 기존 scalar projection이 channel 이상을 반환하는 최소 float 값
[[nodiscard]] float findLinearToSrgbTransition(std::uint8_t channel)
{
    std::uint32_t lowerBits = std::bit_cast<std::uint32_t>(0.0F);
    std::uint32_t upperBits = std::bit_cast<std::uint32_t>(1.0F);
    while (lowerBits < upperBits)
    {
        const std::uint32_t middleBits = lowerBits + (upperBits - lowerBits) / 2;
        if (quantizeLinearToSrgbReference(std::bit_cast<float>(middleBits)) >= channel)
        {
            upperBits = middleBits;
        }
        else
        {
            lowerBits = middleBits + 1;
        }
    }
    return std::bit_cast<float>(lowerBits);
}

// 목적: 8-bit sRGB quantization의 모든 float 전이점을 한 번만 계산
// 입력: 없음
// 출력: channel 0→1부터 254→255까지의 오름차순 linear-light 전이점
[[nodiscard]] std::array<float, LinearToSrgbTransitionCount> buildLinearToSrgbTransitions()
{
    std::array<float, LinearToSrgbTransitionCount> transitions{};
    for (std::size_t index = 0; index < transitions.size(); ++index)
    {
        transitions[index] = findLinearToSrgbTransition(static_cast<std::uint8_t>(index + 1));
    }
    return transitions;
}

const std::array<float, LinearToSrgbTransitionCount> LinearToSrgbTransitions = buildLinearToSrgbTransitions();

// 목적: 16-bit linear bucket마다 기존 sRGB projection의 가까운 channel 값을 미리 계산
// 입력: 없음
// 출력: runtime transition 보정의 시작점으로 사용할 65536-entry table
[[nodiscard]] std::array<std::uint8_t, LinearToSrgbLookupSize> buildLinearToSrgbLookup()
{
    std::array<std::uint8_t, LinearToSrgbLookupSize> lookup{};
    std::size_t transitionIndex = 0;
    for (std::size_t index = 0; index < lookup.size(); ++index)
    {
        const float linearValue = static_cast<float>(index) / LinearToSrgbLookupScale;
        while (transitionIndex < LinearToSrgbTransitions.size() &&
               linearValue >= LinearToSrgbTransitions[transitionIndex])
        {
            ++transitionIndex;
        }
        lookup[index] = static_cast<std::uint8_t>(transitionIndex);
    }
    return lookup;
}

const std::array<std::uint8_t, LinearToSrgbLookupSize> LinearToSrgbLookup = buildLinearToSrgbLookup();

// 목적: 기존 scalar projection과 bit-exact한 8-bit sRGB 값을 direct lookup으로 계산
// 입력: linearValue — 변환할 linear-light channel 값
// 출력: 0~255 범위의 sRGB channel 값
[[nodiscard]] std::uint8_t linearToSrgbChannelValue(float linearValue)
{
    if (linearValue <= 0.0F)
    {
        return 0;
    }
    if (linearValue >= 1.0F)
    {
        return 255;
    }

    const std::size_t lookupIndex = static_cast<std::size_t>(linearValue * LinearToSrgbLookupScale);
    std::uint8_t channel = LinearToSrgbLookup[lookupIndex];
    while (channel > 0 && linearValue < LinearToSrgbTransitions[channel - 1])
    {
        --channel;
    }
    while (channel < 255 && linearValue >= LinearToSrgbTransitions[channel])
    {
        ++channel;
    }
    return channel;
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
// 입력: channel: 원본 8-bit sRGB channel 값, exposureScale/WB gain과 tone parameter
// 출력: 현상된 linear-light channel 값
[[nodiscard]] float applyLightToLinearChannel(std::uint8_t channel,
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
// 출력: pixel channel을 제자리에서 갱신
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
// 출력: pixel channel을 제자리에서 갱신
void applySaturationToLinearPixel(LinearRgb& pixel, float saturation)
{
    scaleLinearPixelChroma(pixel, 1.0F + saturation);
}

// 목적: 저채도 색을 우선해 linear-light RGB pixel의 chroma를 vibrance 조정
// 입력: pixel: 조정할 linear-light RGB 값, vibrance: [-1, 1] vibrance 조정값
// 출력: pixel channel을 제자리에서 갱신
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
// 출력: pixel channel을 제자리에서 갱신
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

// 목적: 두 validated storage view가 byte 범위에서 겹치는지 판정
// 입력: source/destination의 bounded byte span
// 출력: 한 byte라도 같은 storage를 가리키면 true
[[nodiscard]] bool storageRangesOverlap(std::span<const std::uint8_t> source,
                                        std::span<const std::uint8_t> destination) noexcept
{
    const std::uintptr_t sourceAddress = reinterpret_cast<std::uintptr_t>(source.data());
    const std::uintptr_t destinationAddress = reinterpret_cast<std::uintptr_t>(destination.data());
    if (sourceAddress <= destinationAddress)
    {
        return destinationAddress - sourceAddress < source.size();
    }
    return sourceAddress - destinationAddress < destination.size();
}

// 목적: source의 logical RGBA pixel을 stride가 다른 destination에 복사
// 입력: geometry가 같은 non-overlapping source/destination view
// 출력: destination pixel 영역 갱신, row padding은 유지
void copyRgba8Pixels(image::ConstRgba8ImageView source, image::MutableRgba8ImageView destination)
{
    for (std::uint32_t row = 0; row < source.height(); ++row)
    {
        const std::span<const std::uint8_t> sourceRow = source.rowBytes(row);
        std::span<std::uint8_t> destinationRow = destination.rowBytes(row);
        std::copy(sourceRow.begin(), sourceRow.end(), destinationRow.begin());
    }
}

// 목적: pixel-local light/color stage를 source에서 destination으로 적용
// 입력: geometry가 같은 non-overlapping source/destination과 검증된 parameter
// 출력: destination RGB 갱신과 source alpha 보존
void applyPointLocalDevelop(image::ConstRgba8ImageView source,
                            image::MutableRgba8ImageView destination,
                            const types::DevelopParams& params)
{
    const bool hslActive = params.hsl != types::HslParams{};
    const float exposureScale = std::exp2(params.exposureEv);
    const WhiteBalanceGains whiteBalanceGains =
        params.whiteBalanceMode == types::WhiteBalanceMode::Custom
            ? calculateWhiteBalanceGains(params.whiteBalanceTemperatureKelvin, params.whiteBalanceTint)
            : WhiteBalanceGains{};

    std::array<float, SrgbChannelValueCount> redLight{};
    std::array<float, SrgbChannelValueCount> greenLight{};
    std::array<float, SrgbChannelValueCount> blueLight{};
    for (std::size_t channel = 0; channel < SrgbChannelValueCount; ++channel)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(channel);
        redLight[channel] = applyLightToLinearChannel(value, whiteBalanceGains.red, exposureScale, params);
        greenLight[channel] = applyLightToLinearChannel(value, whiteBalanceGains.green, exposureScale, params);
        blueLight[channel] = applyLightToLinearChannel(value, whiteBalanceGains.blue, exposureScale, params);
    }

    for (std::uint32_t row = 0; row < source.height(); ++row)
    {
        const std::span<const std::uint8_t> sourceRow = source.rowBytes(row);
        std::span<std::uint8_t> destinationRow = destination.rowBytes(row);
        for (std::uint32_t column = 0; column < source.width(); ++column)
        {
            const std::size_t pixelOffset = static_cast<std::size_t>(column) * Rgba8BytesPerPixel;
            const std::uint8_t* sourcePixel = sourceRow.data() + pixelOffset;
            std::uint8_t* destinationPixel = destinationRow.data() + pixelOffset;
            LinearRgb linearPixel{
                redLight[sourcePixel[0]],
                greenLight[sourcePixel[1]],
                blueLight[sourcePixel[2]],
            };
            applySaturationToLinearPixel(linearPixel, params.saturation);
            applyVibranceToLinearPixel(linearPixel, params.vibrance);
            applyDehazeToLinearPixel(linearPixel, params.dehaze);
            destinationPixel[0] = linearToSrgbChannelValue(linearPixel.red);
            destinationPixel[1] = linearToSrgbChannelValue(linearPixel.green);
            destinationPixel[2] = linearToSrgbChannelValue(linearPixel.blue);
            destinationPixel[3] = sourcePixel[3];
            if (hslActive)
            {
                detail::applyHslToSrgbPixel(destinationPixel, params.hsl);
            }
        }
    }
}

// 목적: neutral processing 오류 Result 생성
// 입력: code: 실패 원인을 나타내는 error code
// 출력: 지정 오류를 담은 DevelopProcessingResult
[[nodiscard]] DevelopProcessingResult makeProcessingFailure(DevelopProcessingErrorCode code)
{
    return DevelopProcessingResult::failure({code});
}

}  // namespace

namespace detail
{

std::uint8_t projectLinearToSrgbChannel(float linearValue)
{
    return linearToSrgbChannelValue(linearValue);
}

bool hasDevelopAdjustments(const types::DevelopParams& params) noexcept
{
    return params.hsl != types::HslParams{} || params.exposureEv != 0.0F || params.contrast != 0.0F ||
           params.highlights != 0.0F || params.shadows != 0.0F || params.whites != 0.0F || params.blacks != 0.0F ||
           params.saturation != 0.0F || params.vibrance != 0.0F ||
           params.whiteBalanceMode != types::WhiteBalanceMode::AsShot || params.clarity != 0.0F ||
           params.dehaze != 0.0F || params.sharpeningAmount != 0.0F || params.luminanceNoiseReduction != 0.0F ||
           params.colorNoiseReduction != 0.0F || params.toneCurveShadows != 0.0F || params.toneCurveDarks != 0.0F ||
           params.toneCurveLights != 0.0F || params.toneCurveHighlights != 0.0F || params.pointCurveBlack != 0.0F ||
           params.pointCurveShadows != 0.0F || params.pointCurveMidtones != 0.0F ||
           params.pointCurveHighlights != 0.0F || params.pointCurveWhite != 0.0F;
}

bool hasNeighborhoodAdjustments(const types::DevelopParams& params) noexcept
{
    return params.luminanceNoiseReduction != 0.0F || params.colorNoiseReduction != 0.0F || params.clarity != 0.0F ||
           params.sharpeningAmount != 0.0F;
}

}  // namespace detail

DevelopProcessingResult applyDevelop(image::ConstRgba8ImageView source,
                                     image::MutableRgba8ImageView destination,
                                     const types::DevelopParams& params,
                                     int grantedCpuPermits)
{
    if (source.width() != destination.width() || source.height() != destination.height())
    {
        return makeProcessingFailure(DevelopProcessingErrorCode::GeometryMismatch);
    }
    if (storageRangesOverlap(source.bytes(), destination.bytes()))
    {
        return makeProcessingFailure(DevelopProcessingErrorCode::AliasedStorage);
    }
    if (findDevelopParamsValidationIssue(params).has_value())
    {
        return makeProcessingFailure(DevelopProcessingErrorCode::InvalidParameters);
    }
    if (grantedCpuPermits <= 0)
    {
        return makeProcessingFailure(DevelopProcessingErrorCode::InvalidExecutionGrant);
    }

    if (!detail::hasDevelopAdjustments(params))
    {
        copyRgba8Pixels(source, destination);
        return DevelopProcessingResult::success({});
    }

    if (!detail::hasNeighborhoodAdjustments(params))
    {
        applyPointLocalDevelop(source, destination, params);
        return DevelopProcessingResult::success({});
    }

    if (source.width() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        source.height() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        source.rowStride() > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) ||
        destination.rowStride() > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()))
    {
        return makeProcessingFailure(DevelopProcessingErrorCode::UnsupportedGeometry);
    }

    const std::size_t pointRowStride = static_cast<std::size_t>(source.width()) * Rgba8BytesPerPixel;
    std::vector<std::uint8_t> pointStorage(pointRowStride * source.height());
    const std::optional<image::MutableRgba8ImageView> pointDestination =
        image::MutableRgba8ImageView::create(source.width(), source.height(), pointRowStride, pointStorage);
    const std::optional<image::ConstRgba8ImageView> neighborhoodSource =
        image::ConstRgba8ImageView::create(source.width(), source.height(), pointRowStride, pointStorage);
    if (!pointDestination.has_value() || !neighborhoodSource.has_value())
    {
        return makeProcessingFailure(DevelopProcessingErrorCode::UnsupportedGeometry);
    }

    applyPointLocalDevelop(source, *pointDestination, params);
    applyBoundedFloat32Neighborhood(
        {
            .sourcePixels = neighborhoodSource->bytes().data(),
            .sourceBytesPerLine = static_cast<std::ptrdiff_t>(neighborhoodSource->rowStride()),
            .destinationPixels = destination.bytes().data(),
            .destinationBytesPerLine = static_cast<std::ptrdiff_t>(destination.rowStride()),
            .width = static_cast<int>(source.width()),
            .height = static_cast<int>(source.height()),
        },
        {
            .luminanceNoiseReduction = params.luminanceNoiseReduction,
            .colorNoiseReduction = params.colorNoiseReduction,
            .clarity = params.clarity,
            .sharpeningAmount = params.sharpeningAmount,
            .sharpeningRadius = params.sharpeningRadius,
            .sharpeningDetail = params.sharpeningDetail,
            .sharpeningMasking = params.sharpeningMasking,
            .grantedCpuPermits = grantedCpuPermits,
        });
    return DevelopProcessingResult::success({});
}

int maximumUsefulDevelopCpuPermits(int imageHeight, const types::DevelopParams& params)
{
    if (imageHeight <= 0)
    {
        return 1;
    }

    return detail::hasNeighborhoodAdjustments(params) ? boundedFloat32NeighborhoodTaskCount(imageHeight) : 1;
}

}  // namespace flexraw::core::develop

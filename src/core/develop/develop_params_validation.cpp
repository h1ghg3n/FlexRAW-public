#include "develop_params_validation.h"

#include <cmath>

namespace flexraw::core::develop
{
namespace
{

constexpr float MinimumContrast = -1.0F;
constexpr float MaximumContrast = 1.0F;
constexpr float MinimumWhiteBalanceTemperatureKelvin = 2000.0F;
constexpr float MaximumWhiteBalanceTemperatureKelvin = 50000.0F;
constexpr float MinimumWhiteBalanceTint = -1.0F;
constexpr float MaximumWhiteBalanceTint = 1.0F;
constexpr float MinimumClarity = -1.0F;
constexpr float MaximumClarity = 1.0F;
constexpr float MinimumDehaze = -1.0F;
constexpr float MaximumDehaze = 1.0F;
constexpr float MinimumNoiseReduction = 0.0F;
constexpr float MaximumNoiseReduction = 1.0F;
constexpr int MaximumSharpeningKernelRadius = 3;
constexpr float MinimumToneCurve = -1.0F;
constexpr float MaximumToneCurve = 1.0F;

// 목적: 실수 값이 유한하며 지정한 닫힌 구간 안에 있는지 검사
// 입력: value — 검사할 값, minimum/maximum — 허용 구간
// 출력: 유한한 구간 내 값이면 true
[[nodiscard]] bool isFiniteInRange(float value, float minimum, float maximum) noexcept
{
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

}  // namespace

// 목적: Develop parameter에서 첫 번째 허용 범위 위반을 neutral 값으로 식별
// 입력: params — 검사할 Develop parameter 값
// 출력: 유효하면 nullopt, 유효하지 않으면 첫 번째 validation issue
std::optional<DevelopParamsValidationIssue> findDevelopParamsValidationIssue(
    const types::DevelopParams& params) noexcept
{
    if (!std::isfinite(params.exposureEv))
    {
        return DevelopParamsValidationIssue::ExposureNotFinite;
    }
    if (!isFiniteInRange(params.contrast, MinimumContrast, MaximumContrast))
    {
        return DevelopParamsValidationIssue::ContrastOutOfRange;
    }
    if (!isFiniteInRange(params.highlights, MinimumContrast, MaximumContrast))
    {
        return DevelopParamsValidationIssue::HighlightsOutOfRange;
    }
    if (!isFiniteInRange(params.shadows, MinimumContrast, MaximumContrast))
    {
        return DevelopParamsValidationIssue::ShadowsOutOfRange;
    }
    if (!isFiniteInRange(params.whites, MinimumContrast, MaximumContrast))
    {
        return DevelopParamsValidationIssue::WhitesOutOfRange;
    }
    if (!isFiniteInRange(params.blacks, MinimumContrast, MaximumContrast))
    {
        return DevelopParamsValidationIssue::BlacksOutOfRange;
    }
    if (!isFiniteInRange(params.saturation, MinimumContrast, MaximumContrast))
    {
        return DevelopParamsValidationIssue::SaturationOutOfRange;
    }
    if (!isFiniteInRange(params.vibrance, MinimumContrast, MaximumContrast))
    {
        return DevelopParamsValidationIssue::VibranceOutOfRange;
    }
    if (params.whiteBalanceMode != types::WhiteBalanceMode::AsShot &&
        params.whiteBalanceMode != types::WhiteBalanceMode::Custom)
    {
        return DevelopParamsValidationIssue::WhiteBalanceModeInvalid;
    }
    if (!isFiniteInRange(params.whiteBalanceTemperatureKelvin,
                         MinimumWhiteBalanceTemperatureKelvin,
                         MaximumWhiteBalanceTemperatureKelvin))
    {
        return DevelopParamsValidationIssue::WhiteBalanceTemperatureOutOfRange;
    }
    if (!isFiniteInRange(params.whiteBalanceTint, MinimumWhiteBalanceTint, MaximumWhiteBalanceTint))
    {
        return DevelopParamsValidationIssue::WhiteBalanceTintOutOfRange;
    }
    if (!isFiniteInRange(params.clarity, MinimumClarity, MaximumClarity))
    {
        return DevelopParamsValidationIssue::ClarityOutOfRange;
    }
    if (!isFiniteInRange(params.dehaze, MinimumDehaze, MaximumDehaze))
    {
        return DevelopParamsValidationIssue::DehazeOutOfRange;
    }
    if (!isFiniteInRange(params.sharpeningAmount, -1.0F, 1.0F))
    {
        return DevelopParamsValidationIssue::SharpeningAmountOutOfRange;
    }
    if (!isFiniteInRange(params.sharpeningRadius, 1.0F, static_cast<float>(MaximumSharpeningKernelRadius)))
    {
        return DevelopParamsValidationIssue::SharpeningRadiusOutOfRange;
    }
    if (!isFiniteInRange(params.sharpeningDetail, -1.0F, 1.0F))
    {
        return DevelopParamsValidationIssue::SharpeningDetailOutOfRange;
    }
    if (!isFiniteInRange(params.sharpeningMasking, 0.0F, 1.0F))
    {
        return DevelopParamsValidationIssue::SharpeningMaskingOutOfRange;
    }
    if (!isFiniteInRange(params.luminanceNoiseReduction, MinimumNoiseReduction, MaximumNoiseReduction) ||
        !isFiniteInRange(params.colorNoiseReduction, MinimumNoiseReduction, MaximumNoiseReduction))
    {
        return DevelopParamsValidationIssue::NoiseReductionOutOfRange;
    }
    if (!isFiniteInRange(params.toneCurveShadows, MinimumToneCurve, MaximumToneCurve) ||
        !isFiniteInRange(params.toneCurveDarks, MinimumToneCurve, MaximumToneCurve) ||
        !isFiniteInRange(params.toneCurveLights, MinimumToneCurve, MaximumToneCurve) ||
        !isFiniteInRange(params.toneCurveHighlights, MinimumToneCurve, MaximumToneCurve))
    {
        return DevelopParamsValidationIssue::ToneCurveOutOfRange;
    }
    if (!isFiniteInRange(params.pointCurveBlack, MinimumToneCurve, MaximumToneCurve) ||
        !isFiniteInRange(params.pointCurveShadows, MinimumToneCurve, MaximumToneCurve) ||
        !isFiniteInRange(params.pointCurveMidtones, MinimumToneCurve, MaximumToneCurve) ||
        !isFiniteInRange(params.pointCurveHighlights, MinimumToneCurve, MaximumToneCurve) ||
        !isFiniteInRange(params.pointCurveWhite, MinimumToneCurve, MaximumToneCurve))
    {
        return DevelopParamsValidationIssue::PointCurveOutOfRange;
    }

    for (const auto& channel : params.hsl.channels)
    {
        if (!isFiniteInRange(channel.hue, -1.0F, 1.0F) || !isFiniteInRange(channel.saturation, -1.0F, 1.0F) ||
            !isFiniteInRange(channel.luminance, -1.0F, 1.0F))
        {
            return DevelopParamsValidationIssue::HslOutOfRange;
        }
    }
    return std::nullopt;
}

}  // namespace flexraw::core::develop

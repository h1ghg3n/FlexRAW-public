#pragma once

#include <cstdint>
#include <optional>

#include "develop_params.h"

namespace flexraw::core::develop
{

enum class DevelopParamsValidationIssue : std::uint8_t
{
    ExposureNotFinite,
    ContrastOutOfRange,
    HighlightsOutOfRange,
    ShadowsOutOfRange,
    WhitesOutOfRange,
    BlacksOutOfRange,
    SaturationOutOfRange,
    VibranceOutOfRange,
    WhiteBalanceModeInvalid,
    WhiteBalanceTemperatureOutOfRange,
    WhiteBalanceTintOutOfRange,
    ClarityOutOfRange,
    DehazeOutOfRange,
    SharpeningAmountOutOfRange,
    SharpeningRadiusOutOfRange,
    SharpeningDetailOutOfRange,
    SharpeningMaskingOutOfRange,
    NoiseReductionOutOfRange,
    ToneCurveOutOfRange,
    PointCurveOutOfRange,
    HslOutOfRange,
};

// 목적: Develop parameter에서 첫 번째 허용 범위 위반을 neutral 값으로 식별
// 입력: params — 검사할 Develop parameter 값
// 출력: 유효하면 nullopt, 유효하지 않으면 첫 번째 validation issue
[[nodiscard]] std::optional<DevelopParamsValidationIssue> findDevelopParamsValidationIssue(
    const types::DevelopParams& params) noexcept;

}  // namespace flexraw::core::develop

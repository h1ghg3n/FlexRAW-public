#include "develop_validation_adapter.h"

#include <optional>
#include <utility>

#include "develop_params_validation.h"

namespace flexraw::core::develop
{
namespace
{

// 목적: develop 변환 실패를 설명하는 CoreError 값 생성
// 입력: message — 오류 설명
// 출력: InvalidArgument으로 분류된 CoreError 값
[[nodiscard]] types::CoreError makeInvalidArgumentError(QString message)
{
    return types::CoreError{
        types::ErrorCode::InvalidArgument,
        std::move(message),
    };
}

// 목적: neutral validation issue를 기존 CoreError diagnostic 문구로 변환
// 입력: issue — Develop parameter 허용 범위 위반 종류
// 출력: 기존 compatibility API가 사용하는 diagnostic 문구
[[nodiscard]] QString validationIssueMessage(DevelopParamsValidationIssue issue)
{
    switch (issue)
    {
    case DevelopParamsValidationIssue::ExposureNotFinite:
        return QStringLiteral("Develop exposure EV must be finite.");
    case DevelopParamsValidationIssue::ContrastOutOfRange:
        return QStringLiteral("Develop contrast must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::HighlightsOutOfRange:
        return QStringLiteral("Develop highlights must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::ShadowsOutOfRange:
        return QStringLiteral("Develop shadows must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::WhitesOutOfRange:
        return QStringLiteral("Develop whites must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::BlacksOutOfRange:
        return QStringLiteral("Develop blacks must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::SaturationOutOfRange:
        return QStringLiteral("Develop saturation must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::VibranceOutOfRange:
        return QStringLiteral("Develop vibrance must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::WhiteBalanceModeInvalid:
        return QStringLiteral("Develop white balance mode is invalid.");
    case DevelopParamsValidationIssue::WhiteBalanceTemperatureOutOfRange:
        return QStringLiteral("Develop white balance temperature must be between 2000 and 50000 Kelvin.");
    case DevelopParamsValidationIssue::WhiteBalanceTintOutOfRange:
        return QStringLiteral("Develop white balance tint must be between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::ClarityOutOfRange:
        return QStringLiteral("Develop clarity must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::DehazeOutOfRange:
        return QStringLiteral("Develop dehaze must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::SharpeningAmountOutOfRange:
        return QStringLiteral("Develop sharpening amount is invalid.");
    case DevelopParamsValidationIssue::SharpeningRadiusOutOfRange:
        return QStringLiteral("Develop sharpening radius must be between 1.0 and 3.0.");
    case DevelopParamsValidationIssue::SharpeningDetailOutOfRange:
        return QStringLiteral("Develop sharpening detail must be between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::SharpeningMaskingOutOfRange:
        return QStringLiteral("Develop sharpening masking must be between 0.0 and 1.0.");
    case DevelopParamsValidationIssue::NoiseReductionOutOfRange:
        return QStringLiteral("Develop noise reduction values must be between 0.0 and 1.0.");
    case DevelopParamsValidationIssue::ToneCurveOutOfRange:
        return QStringLiteral("Develop tone curve values must be between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::HslOutOfRange:
        return QStringLiteral("Develop HSL values must be finite and between -1.0 and 1.0.");
    case DevelopParamsValidationIssue::PointCurveOutOfRange:
        return QStringLiteral("Develop point curve values must be between -1.0 and 1.0.");
    }

    return QStringLiteral("Develop parameters are invalid.");
}

}  // namespace

// 목적: Develop parameter 전체를 검사하고 기존 CoreError compatibility 결과로 변환
// 입력: params — 검사할 Develop parameter 값
// 출력: 유효한 parameter 복사본 또는 기존 diagnostic을 보존한 구조화된 오류
DevelopParamsValidationResult validateDevelopParams(const types::DevelopParams& params)
{
    const std::optional<DevelopParamsValidationIssue> issue = findDevelopParamsValidationIssue(params);
    if (issue.has_value())
    {
        return DevelopParamsValidationResult::failure(makeInvalidArgumentError(validationIssueMessage(*issue)));
    }

    return DevelopParamsValidationResult::success(params);
}

}  // namespace flexraw::core::develop

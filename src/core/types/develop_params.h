#pragma once

namespace flexraw::core::types
{

enum class WhiteBalanceMode
{
    AsShot,
    Custom,
};

struct DevelopParams
{
    float exposureEv{0.0F};
    float contrast{0.0F};
    float highlights{0.0F};
    float shadows{0.0F};
    float whites{0.0F};
    float blacks{0.0F};
    float saturation{0.0F};
    float vibrance{0.0F};
    WhiteBalanceMode whiteBalanceMode{WhiteBalanceMode::AsShot};
    float whiteBalanceTemperatureKelvin{6500.0F};
    float whiteBalanceTint{0.0F};
    float clarity{0.0F};
    float dehaze{0.0F};
    float sharpeningAmount{0.0F};
    float sharpeningRadius{1.0F};
    float sharpeningDetail{0.0F};
    float sharpeningMasking{0.0F};
    float luminanceNoiseReduction{0.0F};
    float colorNoiseReduction{0.0F};
    float toneCurveShadows{0.0F};
    float toneCurveDarks{0.0F};
    float toneCurveLights{0.0F};
    float toneCurveHighlights{0.0F};
    float pointCurveBlack{0.0F};
    float pointCurveShadows{0.0F};
    float pointCurveMidtones{0.0F};
    float pointCurveHighlights{0.0F};
    float pointCurveWhite{0.0F};

    // 목적: 두 develop parameter 집합의 모든 값을 비교
    // 입력: other: 비교할 develop parameter 값
    // 출력: 모든 parameter가 같으면 true
    [[nodiscard]] bool operator==(const DevelopParams& other) const = default;
};

}  // namespace flexraw::core::types

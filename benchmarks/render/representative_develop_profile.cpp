#include "representative_develop_profile.h"

namespace flexraw::benchmarks::render
{

// 목적: image regression candidate에 고정할 대표 Develop parameter 생성
// 입력: 없음
// 출력: 주요 Develop stage를 활성화하는 version 1 DevelopParams
core::types::DevelopParams makeRepresentativeDevelopParams()
{
    core::types::DevelopParams params;
    params.exposureEv = 0.35F;
    params.contrast = 0.20F;
    params.highlights = -0.25F;
    params.shadows = 0.25F;
    params.whites = 0.10F;
    params.blacks = -0.10F;
    params.saturation = 0.10F;
    params.vibrance = 0.20F;
    params.whiteBalanceMode = core::types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 6200.0F;
    params.whiteBalanceTint = 0.05F;
    params.clarity = 0.15F;
    params.dehaze = 0.10F;
    params.sharpeningAmount = 0.25F;
    params.sharpeningRadius = 1.2F;
    params.sharpeningDetail = 0.20F;
    params.sharpeningMasking = 0.10F;
    params.luminanceNoiseReduction = 0.15F;
    params.colorNoiseReduction = 0.15F;
    params.toneCurveShadows = -0.05F;
    params.toneCurveDarks = -0.05F;
    params.toneCurveLights = 0.05F;
    params.toneCurveHighlights = 0.05F;
    return params;
}

}  // namespace flexraw::benchmarks::render

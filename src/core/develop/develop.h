#pragma once

#include <QImage>

#include "develop_params.h"
#include "error.h"
#include "result.h"

namespace flexraw::core::develop
{

using DevelopImageResult = types::Result<QImage, types::CoreError>;
using DevelopParamsValidationResult = types::Result<types::DevelopParams, types::CoreError>;

// 목적: develop parameter 전체가 Core 허용 범위에 있는지 검증
// 입력: params: 검증할 develop parameter 값
// 출력: 유효한 parameter 복사본 또는 구조화된 오류
[[nodiscard]] DevelopParamsValidationResult validateDevelopParams(const types::DevelopParams& params);

// 목적: sRGB preview image에 non-destructive develop parameter를 적용
// 입력: sourceImage: sRGB preview 원본, params: 적용할 develop parameter 값
// 출력: 현상된 sRGB QImage 또는 구조화된 오류
// 에러 처리: null image와 유효하지 않은 develop parameter는 Result, 메모리 부족은 예외
[[nodiscard]] DevelopImageResult applyDevelop(const QImage& sourceImage, const types::DevelopParams& params);

}  // namespace flexraw::core::develop

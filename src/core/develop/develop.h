#pragma once

#include "develop_qt_adapter.h"
#include "develop_validation_adapter.h"

namespace flexraw::core::develop
{

// 목적: sRGB preview image에 non-destructive develop parameter를 적용
// 입력: sourceImage: sRGB preview 원본, params: 적용할 develop parameter 값
// 출력: 현상된 sRGB QImage 또는 구조화된 오류
// 에러 처리: null image와 유효하지 않은 develop parameter는 Result, 메모리 부족은 예외
[[nodiscard]] DevelopImageResult applyDevelop(const QImage& sourceImage, const types::DevelopParams& params);

// 목적: 명시적으로 허가된 CPU permit 범위에서 sRGB preview image에 develop parameter 적용
// 입력: sourceImage/params — 현상 입력, grantedCpuPermits — caller가 대여한 compute permit 수
// 출력: 현상된 sRGB QImage 또는 구조화된 오류
// 에러 처리: null image, 유효하지 않은 parameter 또는 permit 수는 Result, 메모리 부족은 예외
[[nodiscard]] DevelopImageResult applyDevelop(const QImage& sourceImage,
                                              const types::DevelopParams& params,
                                              int grantedCpuPermits);

}  // namespace flexraw::core::develop

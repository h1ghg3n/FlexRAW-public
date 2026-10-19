#pragma once

#include <QImage>

#include "develop_params.h"
#include "error.h"
#include "result.h"

namespace flexraw::core::develop
{

using DevelopImageResult = types::Result<QImage, types::CoreError>;

namespace qt_adapter
{

// 목적: Qt QImage storage를 neutral RGBA8 Develop contract에 맞춰 처리
// 입력: sourceImage/params — Qt image와 보정값, grantedCpuPermits — caller가 허가한 CPU 상한
// 출력: 현상된 sRGB QImage 또는 기존 CoreError 의미
// 에러 처리: null image, 유효하지 않은 parameter 또는 permit 수는 Result, 메모리 부족은 예외
[[nodiscard]] DevelopImageResult apply(const QImage& sourceImage,
                                       const types::DevelopParams& params,
                                       int grantedCpuPermits = 1);

}  // namespace qt_adapter
}  // namespace flexraw::core::develop

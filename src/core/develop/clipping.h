#pragma once

#include <QImage>

#include "error.h"
#include "result.h"

namespace flexraw::core::develop
{

struct ClippingSummary
{
    quint64 shadowPixelCount{0};
    quint64 highlightPixelCount{0};
    quint64 pixelCount{0};
};

using ClippingSummaryResult = types::Result<ClippingSummary, types::CoreError>;

// 목적: sRGB preview image에서 shadow와 highlight clipping pixel 수 계산
// 입력: sourceImage: 분석할 preview 이미지
// 출력: clipping pixel 수 또는 빈 이미지 오류
[[nodiscard]] ClippingSummaryResult calculateClippingSummary(const QImage& sourceImage);

}  // namespace flexraw::core::develop

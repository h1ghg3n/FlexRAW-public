#pragma once

#include <array>

#include <QImage>

#include "error.h"
#include "result.h"

namespace flexraw::core::develop
{

using HistogramBins = std::array<quint32, 256>;

struct ImageHistogram
{
    HistogramBins red{};
    HistogramBins green{};
    HistogramBins blue{};
    HistogramBins luminance{};
    quint64 pixelCount{0};
};

using ImageHistogramResult = types::Result<ImageHistogram, types::CoreError>;

// 목적: sRGB preview image의 RGB와 Rec.709 휘도 histogram 계산
// 입력: sourceImage: 분석할 preview 이미지
// 출력: 256-bin channel histogram 또는 빈 이미지 오류
[[nodiscard]] ImageHistogramResult calculateImageHistogram(const QImage& sourceImage);

}  // namespace flexraw::core::develop

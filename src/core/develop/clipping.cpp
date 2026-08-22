#include "clipping.h"

namespace flexraw::core::develop
{
namespace
{

constexpr uchar ShadowClipThreshold = 2;
constexpr uchar HighlightClipThreshold = 253;

// 목적: clipping 분석 실패를 설명하는 InvalidArgument 오류 생성
// 입력: message: 호출자에게 전달할 오류 설명
// 출력: InvalidArgument로 분류된 CoreError 값
[[nodiscard]] types::CoreError makeInvalidArgumentError(const QString& message)
{
    return {types::ErrorCode::InvalidArgument, message};
}

// 목적: RGB pixel이 shadow clipping 기준을 만족하는지 판정
// 입력: red/green/blue: 0~255 범위의 sRGB encoded channel 값
// 출력: 모든 channel이 shadow threshold 이하이면 true
[[nodiscard]] bool isShadowClipped(uchar red, uchar green, uchar blue)
{
    return red <= ShadowClipThreshold && green <= ShadowClipThreshold && blue <= ShadowClipThreshold;
}

// 목적: RGB pixel이 highlight clipping 기준을 만족하는지 판정
// 입력: red/green/blue: 0~255 범위의 sRGB encoded channel 값
// 출력: 하나 이상의 channel이 highlight threshold 이상이면 true
[[nodiscard]] bool isHighlightClipped(uchar red, uchar green, uchar blue)
{
    return red >= HighlightClipThreshold || green >= HighlightClipThreshold || blue >= HighlightClipThreshold;
}

}  // namespace

// 목적: sRGB preview image에서 shadow와 highlight clipping pixel 수 계산
// 입력: sourceImage: 분석할 preview 이미지
// 출력: clipping pixel 수 또는 빈 이미지 오류
ClippingSummaryResult calculateClippingSummary(const QImage& sourceImage)
{
    if (sourceImage.isNull())
    {
        return ClippingSummaryResult::failure(
            makeInvalidArgumentError(QStringLiteral("Clipping source image is null.")));
    }

    const QImage rgbaImage = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    if (rgbaImage.isNull())
    {
        return ClippingSummaryResult::failure(
            makeInvalidArgumentError(QStringLiteral("Unable to convert clipping source image.")));
    }

    ClippingSummary summary;
    summary.pixelCount = static_cast<quint64>(rgbaImage.width()) * static_cast<quint64>(rgbaImage.height());

    for (int row = 0; row < rgbaImage.height(); ++row)
    {
        const uchar* pixels = rgbaImage.constScanLine(row);

        for (int column = 0; column < rgbaImage.width(); ++column)
        {
            const uchar* pixel = pixels + column * 4;
            if (isShadowClipped(pixel[0], pixel[1], pixel[2]))
            {
                ++summary.shadowPixelCount;
            }

            if (isHighlightClipped(pixel[0], pixel[1], pixel[2]))
            {
                ++summary.highlightPixelCount;
            }
        }
    }

    return ClippingSummaryResult::success(summary);
}

}  // namespace flexraw::core::develop

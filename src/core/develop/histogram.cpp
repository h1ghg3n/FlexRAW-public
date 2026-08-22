#include "histogram.h"

#include <utility>

namespace flexraw::core::develop
{
namespace
{

constexpr int RedLuminanceWeight = 54;
constexpr int GreenLuminanceWeight = 183;
constexpr int BlueLuminanceWeight = 19;
constexpr int LuminanceWeightSum = RedLuminanceWeight + GreenLuminanceWeight + BlueLuminanceWeight;

// 목적: histogram 계산 실패를 설명하는 InvalidArgument 오류 생성
// 입력: message: 호출자에게 전달할 오류 설명
// 출력: InvalidArgument로 분류된 CoreError 값
[[nodiscard]] types::CoreError makeInvalidArgumentError(const QString& message)
{
    return {types::ErrorCode::InvalidArgument, message};
}

// 목적: sRGB encoded RGB channel에서 Rec.709 근사 휘도 bin을 계산
// 입력: red/green/blue: 0~255 범위의 sRGB encoded channel 값
// 출력: 0~255 범위의 휘도 bin
[[nodiscard]] int calculateLuminanceBin(int red, int green, int blue)
{
    return (red * RedLuminanceWeight + green * GreenLuminanceWeight + blue * BlueLuminanceWeight +
            LuminanceWeightSum / 2) /
           LuminanceWeightSum;
}

}  // namespace

// 목적: sRGB preview image의 RGB와 Rec.709 휘도 histogram 계산
// 입력: sourceImage: 분석할 preview 이미지
// 출력: 256-bin channel histogram 또는 빈 이미지 오류
ImageHistogramResult calculateImageHistogram(const QImage& sourceImage)
{
    if (sourceImage.isNull())
    {
        return ImageHistogramResult::failure(
            makeInvalidArgumentError(QStringLiteral("Histogram source image is null.")));
    }

    const QImage rgbaImage = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    if (rgbaImage.isNull())
    {
        return ImageHistogramResult::failure(
            makeInvalidArgumentError(QStringLiteral("Unable to convert histogram source image.")));
    }

    ImageHistogram histogram;
    histogram.pixelCount = static_cast<quint64>(rgbaImage.width()) * static_cast<quint64>(rgbaImage.height());

    for (int row = 0; row < rgbaImage.height(); ++row)
    {
        const uchar* pixels = rgbaImage.constScanLine(row);

        for (int column = 0; column < rgbaImage.width(); ++column)
        {
            const uchar* pixel = pixels + column * 4;
            ++histogram.red[pixel[0]];
            ++histogram.green[pixel[1]];
            ++histogram.blue[pixel[2]];
            ++histogram.luminance[calculateLuminanceBin(pixel[0], pixel[1], pixel[2])];
        }
    }

    return ImageHistogramResult::success(std::move(histogram));
}

}  // namespace flexraw::core::develop

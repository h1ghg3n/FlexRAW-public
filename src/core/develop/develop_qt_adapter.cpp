#include "develop_qt_adapter.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <utility>

#include "develop_processing.h"
#include "develop_processing_detail.h"
#include "develop_validation_adapter.h"

namespace flexraw::core::develop::qt_adapter
{
namespace
{

// 목적: Qt compatibility 변환 실패를 기존 CoreError 의미로 생성
// 입력: message: diagnostic 오류 설명
// 출력: InvalidArgument으로 분류된 CoreError 값
[[nodiscard]] types::CoreError makeInvalidArgumentError(QString message)
{
    return types::CoreError{
        types::ErrorCode::InvalidArgument,
        std::move(message),
    };
}

}  // namespace

// 목적: Qt QImage storage를 neutral RGBA8 Develop contract에 맞춰 처리
// 입력: sourceImage/params — Qt image와 보정값, grantedCpuPermits — caller가 허가한 CPU 상한
// 출력: 현상된 sRGB QImage 또는 기존 CoreError 의미
// 에러 처리: null image, 유효하지 않은 parameter 또는 permit 수는 Result, 메모리 부족은 예외
DevelopImageResult apply(const QImage& sourceImage, const types::DevelopParams& params, int grantedCpuPermits)
{
    if (sourceImage.isNull())
    {
        return DevelopImageResult::failure(makeInvalidArgumentError(QStringLiteral("Develop source image is null.")));
    }

    const DevelopParamsValidationResult validation = validateDevelopParams(params);
    if (validation.hasError())
    {
        return DevelopImageResult::failure(validation.error());
    }

    if (grantedCpuPermits <= 0)
    {
        return DevelopImageResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop CPU permit count must be positive.")));
    }

    if (!detail::hasDevelopAdjustments(params))
    {
        return DevelopImageResult::success(sourceImage);
    }

    const QImage normalizedSource = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    if (normalizedSource.isNull())
    {
        return DevelopImageResult::failure(
            makeInvalidArgumentError(QStringLiteral("Unable to convert develop source image.")));
    }

    QImage developedImage(normalizedSource.size(), QImage::Format_RGBA8888);
    std::uint8_t* developedPixels = reinterpret_cast<std::uint8_t*>(developedImage.bits());
    if (developedPixels == nullptr)
    {
        throw std::bad_alloc();
    }

    const std::optional<image::ConstRgba8ImageView> sourceView = image::ConstRgba8ImageView::create(
        static_cast<std::uint32_t>(normalizedSource.width()),
        static_cast<std::uint32_t>(normalizedSource.height()),
        static_cast<std::size_t>(normalizedSource.bytesPerLine()),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(normalizedSource.constBits()),
                                      static_cast<std::size_t>(normalizedSource.sizeInBytes())));
    const std::optional<image::MutableRgba8ImageView> destinationView = image::MutableRgba8ImageView::create(
        static_cast<std::uint32_t>(developedImage.width()),
        static_cast<std::uint32_t>(developedImage.height()),
        static_cast<std::size_t>(developedImage.bytesPerLine()),
        std::span<std::uint8_t>(developedPixels, static_cast<std::size_t>(developedImage.sizeInBytes())));
    if (!sourceView.has_value() || !destinationView.has_value())
    {
        return DevelopImageResult::failure(
            makeInvalidArgumentError(QStringLiteral("Unable to create the Develop RGBA8 image view.")));
    }

    const DevelopProcessingResult processed =
        develop::applyDevelop(*sourceView, *destinationView, params, grantedCpuPermits);
    if (processed.hasError())
    {
        switch (processed.error().code)
        {
        case DevelopProcessingErrorCode::InvalidParameters:
            return DevelopImageResult::failure(
                makeInvalidArgumentError(QStringLiteral("Develop parameters are invalid.")));
        case DevelopProcessingErrorCode::InvalidExecutionGrant:
            return DevelopImageResult::failure(
                makeInvalidArgumentError(QStringLiteral("Develop CPU permit count must be positive.")));
        case DevelopProcessingErrorCode::GeometryMismatch:
            return DevelopImageResult::failure(
                makeInvalidArgumentError(QStringLiteral("Develop source and destination geometry must match.")));
        case DevelopProcessingErrorCode::AliasedStorage:
            return DevelopImageResult::failure(
                makeInvalidArgumentError(QStringLiteral("Develop source and destination storage must not overlap.")));
        case DevelopProcessingErrorCode::UnsupportedGeometry:
            return DevelopImageResult::failure(
                makeInvalidArgumentError(QStringLiteral("Develop image geometry is unsupported.")));
        }
        return DevelopImageResult::failure(
            makeInvalidArgumentError(QStringLiteral("Develop processing failed with an unknown error.")));
    }

    return DevelopImageResult::success(std::move(developedImage));
}

}  // namespace flexraw::core::develop::qt_adapter

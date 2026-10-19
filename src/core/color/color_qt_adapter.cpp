#include "color_qt_adapter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include <QColorSpace>

#include "color_processing.h"

namespace flexraw::core::color::qt_adapter
{
namespace
{

// 목적: Color Qt adapter 실패 정보를 일관된 CoreError로 생성
// 입력: code: 실패 분류, message: diagnostic 설명
// 출력: 구성된 CoreError 객체
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: neutral Color processing 오류를 기존 CoreError 의미로 투영
// 입력: error: neutral processing 오류
// 출력: 기존 caller가 소비하는 error code와 diagnostic message
[[nodiscard]] types::CoreError projectProcessingError(ColorProcessingError error)
{
    switch (error.code)
    {
    case ColorProcessingErrorCode::ProfileCreationFailed:
        return makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to create the sRGB ICC profile."));
    case ColorProcessingErrorCode::ProfileSerializationFailed:
        return makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to serialize the sRGB ICC profile."));
    case ColorProcessingErrorCode::EmptyProfile:
        return makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Both ICC profiles must be provided."));
    case ColorProcessingErrorCode::ProfileTooLarge:
        return makeError(types::ErrorCode::InvalidArgument, QStringLiteral("The ICC profile is too large."));
    case ColorProcessingErrorCode::InvalidSourceProfile:
        return makeError(types::ErrorCode::DecodeFailed, QStringLiteral("The source ICC profile is invalid."));
    case ColorProcessingErrorCode::InvalidDestinationProfile:
        return makeError(types::ErrorCode::DecodeFailed, QStringLiteral("The destination ICC profile is invalid."));
    case ColorProcessingErrorCode::TransformCreationFailed:
        return makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to create the ICC color transform."));
    case ColorProcessingErrorCode::GeometryMismatch:
        return makeError(types::ErrorCode::InvalidArgument,
                         QStringLiteral("The source and destination image geometry must match."));
    case ColorProcessingErrorCode::AliasedStorage:
        return makeError(types::ErrorCode::InvalidArgument,
                         QStringLiteral("The source and destination image storage must not overlap."));
    case ColorProcessingErrorCode::UnsupportedGeometry:
        return makeError(types::ErrorCode::InvalidArgument, QStringLiteral("The source image is too large."));
    }
    return makeError(types::ErrorCode::Unknown, QStringLiteral("Color processing failed with an unknown error."));
}

// 목적: 유효한 Qt 표준 색 공간에서 직렬화 가능한 ICC profile byte data 추출
// 입력: colorSpace: ICC profile을 제공하는 유효한 Qt 색 공간
// 출력: ICC profile byte data 또는 추출 실패 정보
[[nodiscard]] IccProfileResult serializeQtIccProfile(const QColorSpace& colorSpace)
{
    const QByteArray profileData = colorSpace.iccProfile();
    if (profileData.isEmpty())
    {
        return IccProfileResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to serialize the RGB ICC profile.")));
    }
    return IccProfileResult::success(profileData);
}

}  // namespace

// 목적: LittleCMS 가 제공하는 표준 sRGB ICC profile을 QByteArray로 투영
// 입력: 없음
// 출력: ICC profile byte data 또는 생성 실패 정보
IccProfileResult createSrgbIccProfile()
{
    IccProfileDataResult profile = createSrgbIccProfileData();
    if (profile.hasError())
    {
        return IccProfileResult::failure(projectProcessingError(profile.error()));
    }
    if (profile.value().size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return IccProfileResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("The ICC profile is too large.")));
    }

    return IccProfileResult::success(QByteArray(reinterpret_cast<const char*>(profile.value().data()),
                                                static_cast<qsizetype>(profile.value().size())));
}

// 목적: 지정한 표준 RGB 색 공간의 ICC profile을 QByteArray로 투영
// 입력: colorSpace: sRGB, Adobe RGB 또는 Display P3 표준 색 공간
// 출력: ICC profile byte data 또는 지원하지 않는 색 공간/생성 실패 정보
IccProfileResult createRgbIccProfile(RgbColorSpace colorSpace)
{
    switch (colorSpace)
    {
    case RgbColorSpace::Srgb:
        return createSrgbIccProfile();
    case RgbColorSpace::AdobeRgb:
        return serializeQtIccProfile(QColorSpace(QColorSpace::AdobeRgb));
    case RgbColorSpace::DisplayP3:
        return serializeQtIccProfile(QColorSpace(QColorSpace::DisplayP3));
    }

    return IccProfileResult::failure(
        makeError(types::ErrorCode::UnsupportedFormat, QStringLiteral("The RGB color space is unsupported.")));
}

// 목적: QImage를 neutral ICC RGBA8 processing에 연결
// 입력: sourceImage: 변환할 이미지, sourceProfileData/destinationProfileData: ICC profile byte data
// 출력: 변환된 RGBA image 또는 입력/profile/변환 실패 정보
ColorImageResult transformIccImage(const QImage& sourceImage,
                                   const QByteArray& sourceProfileData,
                                   const QByteArray& destinationProfileData)
{
    if (sourceImage.isNull())
    {
        return ColorImageResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("The source image is empty.")));
    }

    const QImage sourceRgba = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    QImage destinationImage(sourceRgba.size(), QImage::Format_RGBA8888);
    if (sourceRgba.isNull() || destinationImage.isNull())
    {
        return ColorImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to allocate the destination image.")));
    }

    const std::optional<image::ConstRgba8ImageView> sourceView = image::ConstRgba8ImageView::create(
        static_cast<std::uint32_t>(sourceRgba.width()),
        static_cast<std::uint32_t>(sourceRgba.height()),
        static_cast<std::size_t>(sourceRgba.bytesPerLine()),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(sourceRgba.constBits()),
                                      static_cast<std::size_t>(sourceRgba.sizeInBytes())));
    const std::optional<image::MutableRgba8ImageView> destinationView = image::MutableRgba8ImageView::create(
        static_cast<std::uint32_t>(destinationImage.width()),
        static_cast<std::uint32_t>(destinationImage.height()),
        static_cast<std::size_t>(destinationImage.bytesPerLine()),
        std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(destinationImage.bits()),
                                static_cast<std::size_t>(destinationImage.sizeInBytes())));
    if (!sourceView.has_value() || !destinationView.has_value())
    {
        return ColorImageResult::failure(makeError(types::ErrorCode::InvalidArgument,
                                                   QStringLiteral("Unable to create the Color RGBA8 image view.")));
    }

    const std::span<const std::uint8_t> sourceProfile(
        reinterpret_cast<const std::uint8_t*>(sourceProfileData.constData()),
        static_cast<std::size_t>(sourceProfileData.size()));
    const std::span<const std::uint8_t> destinationProfile(
        reinterpret_cast<const std::uint8_t*>(destinationProfileData.constData()),
        static_cast<std::size_t>(destinationProfileData.size()));
    const ColorProcessingResult transformed =
        transformIccRgba8(*sourceView, *destinationView, sourceProfile, destinationProfile);
    if (transformed.hasError())
    {
        return ColorImageResult::failure(projectProcessingError(transformed.error()));
    }
    return ColorImageResult::success(std::move(destinationImage));
}

}  // namespace flexraw::core::color::qt_adapter

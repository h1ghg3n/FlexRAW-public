#include "color.h"

#include <limits>
#include <utility>

#include <lcms2.h>

#include <QColorSpace>

namespace flexraw::core::color
{
namespace
{

// 목적: 색상 변환 실패 정보를 일관된 CoreError 로 생성
// 입력: code — 실패 분류, message — 사용자/호출자에게 전달할 설명
// 출력: 구성된 CoreError 객체
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, const QString& message)
{
    return {code, message};
}

// 목적: 유효한 Qt 표준 색 공간에서 직렬화 가능한 ICC 프로필 byte data를 추출
// 입력: colorSpace: ICC 프로필을 제공하는 유효한 Qt 색 공간
// 출력: ICC 프로필 byte data 또는 추출 실패 정보
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

// 목적: LittleCMS 가 제공하는 표준 sRGB ICC 프로필을 직렬화해 반환
// 입력: 없음
// 출력: ICC 프로필 byte data 또는 생성 실패 정보
IccProfileResult createSrgbIccProfile()
{
    cmsHPROFILE profile = cmsCreate_sRGBProfile();
    if (profile == nullptr)
    {
        return IccProfileResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to create the sRGB ICC profile.")));
    }

    cmsUInt32Number profileSize = 0;
    if (cmsSaveProfileToMem(profile, nullptr, &profileSize) == 0 || profileSize == 0)
    {
        cmsCloseProfile(profile);
        return IccProfileResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to serialize the sRGB ICC profile.")));
    }

    QByteArray profileData(static_cast<qsizetype>(profileSize), Qt::Uninitialized);
    const bool saved = cmsSaveProfileToMem(profile, profileData.data(), &profileSize) != 0;
    cmsCloseProfile(profile);

    if (!saved)
    {
        return IccProfileResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to serialize the sRGB ICC profile.")));
    }

    return IccProfileResult::success(std::move(profileData));
}

// 목적: 지정한 표준 RGB 색 공간의 ICC 프로필을 직렬화해 반환
// 입력: colorSpace: sRGB, Adobe RGB 또는 Display P3 표준 색 공간
// 출력: ICC 프로필 byte data 또는 지원하지 않는 색 공간/생성 실패 정보
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

// 목적: 입력 ICC 프로필에서 출력 ICC 프로필로 RGBA 이미지를 변환
// 입력: sourceImage: 변환할 이미지, sourceProfileData/destinationProfileData: ICC 프로필 byte data
// 출력: 변환된 RGBA 이미지 또는 입력/프로필/변환 실패 정보
ColorImageResult transformIccImage(const QImage& sourceImage,
                                   const QByteArray& sourceProfileData,
                                   const QByteArray& destinationProfileData)
{
    if (sourceImage.isNull())
    {
        return ColorImageResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("The source image is empty.")));
    }

    if (sourceProfileData.isEmpty() || destinationProfileData.isEmpty())
    {
        return ColorImageResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Both ICC profiles must be provided.")));
    }

    constexpr auto maxProfileSize = std::numeric_limits<cmsUInt32Number>::max();
    if (sourceProfileData.size() > maxProfileSize || destinationProfileData.size() > maxProfileSize)
    {
        return ColorImageResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("The ICC profile is too large.")));
    }

    const quint64 pixelCount = static_cast<quint64>(sourceImage.width()) * static_cast<quint64>(sourceImage.height());
    if (pixelCount > std::numeric_limits<cmsUInt32Number>::max())
    {
        return ColorImageResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("The source image is too large.")));
    }

    cmsHPROFILE sourceProfile =
        cmsOpenProfileFromMem(sourceProfileData.constData(), static_cast<cmsUInt32Number>(sourceProfileData.size()));
    if (sourceProfile == nullptr)
    {
        return ColorImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("The source ICC profile is invalid.")));
    }

    cmsHPROFILE destinationProfile = cmsOpenProfileFromMem(destinationProfileData.constData(),
                                                           static_cast<cmsUInt32Number>(destinationProfileData.size()));
    if (destinationProfile == nullptr)
    {
        cmsCloseProfile(sourceProfile);
        return ColorImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("The destination ICC profile is invalid.")));
    }

    cmsHTRANSFORM transform = cmsCreateTransform(
        sourceProfile, TYPE_RGBA_8, destinationProfile, TYPE_RGBA_8, INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);
    if (transform == nullptr)
    {
        cmsCloseProfile(destinationProfile);
        cmsCloseProfile(sourceProfile);
        return ColorImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to create the ICC color transform.")));
    }

    const QImage sourceRgba = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    QImage destinationImage(sourceRgba.size(), QImage::Format_RGBA8888);
    if (destinationImage.isNull())
    {
        cmsDeleteTransform(transform);
        cmsCloseProfile(destinationProfile);
        cmsCloseProfile(sourceProfile);
        return ColorImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to allocate the destination image.")));
    }

    cmsDoTransform(
        transform, sourceRgba.constBits(), destinationImage.bits(), static_cast<cmsUInt32Number>(pixelCount));
    cmsDeleteTransform(transform);
    cmsCloseProfile(destinationProfile);
    cmsCloseProfile(sourceProfile);

    return ColorImageResult::success(std::move(destinationImage));
}

}  // namespace flexraw::core::color

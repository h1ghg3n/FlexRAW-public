#include "color_processing.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <lcms2.h>

namespace flexraw::core::color
{
namespace
{

constexpr std::size_t Rgba8BytesPerPixel = 4;

struct ProfileDeleter
{
    // 목적: 소유한 LittleCMS profile handle 반환
    // 입력: profile: null일 수 있는 LittleCMS profile handle
    // 출력: 없음
    void operator()(void* profile) const noexcept
    {
        if (profile != nullptr)
        {
            cmsCloseProfile(profile);
        }
    }
};

struct TransformDeleter
{
    // 목적: 소유한 LittleCMS transform handle 반환
    // 입력: transform: null일 수 있는 LittleCMS transform handle
    // 출력: 없음
    void operator()(void* transform) const noexcept
    {
        if (transform != nullptr)
        {
            cmsDeleteTransform(transform);
        }
    }
};

using ProfileHandle = std::unique_ptr<void, ProfileDeleter>;
using TransformHandle = std::unique_ptr<void, TransformDeleter>;

// 목적: neutral Color processing 오류 생성
// 입력: code: 실패 분류
// 출력: 지정 code를 가진 오류 값
[[nodiscard]] ColorProcessingError makeError(ColorProcessingErrorCode code) noexcept
{
    return ColorProcessingError{code};
}

// 목적: source와 destination backing storage가 일부라도 겹치는지 overflow 없이 확인
// 입력: source/destination: 비교할 byte storage
// 출력: storage range가 겹치거나 주소 범위 계산이 안전하지 않으면 true
[[nodiscard]] bool rangesOverlap(std::span<const std::uint8_t> source, std::span<std::uint8_t> destination) noexcept
{
    const std::uintptr_t sourceBegin = reinterpret_cast<std::uintptr_t>(source.data());
    const std::uintptr_t destinationBegin = reinterpret_cast<std::uintptr_t>(destination.data());
    if (source.size() > std::numeric_limits<std::uintptr_t>::max() - sourceBegin ||
        destination.size() > std::numeric_limits<std::uintptr_t>::max() - destinationBegin)
    {
        return true;
    }

    const std::uintptr_t sourceEnd = sourceBegin + source.size();
    const std::uintptr_t destinationEnd = destinationBegin + destination.size();
    return sourceBegin < destinationEnd && destinationBegin < sourceEnd;
}

}  // namespace

// 목적: LittleCMS 표준 sRGB ICC profile을 project-owned byte storage로 직렬화
// 입력: 없음
// 출력: ICC profile byte data 또는 neutral processing 오류, 메모리 부족은 예외
IccProfileDataResult createSrgbIccProfileData()
{
    const ProfileHandle profile(cmsCreate_sRGBProfile());
    if (profile == nullptr)
    {
        return IccProfileDataResult::failure(makeError(ColorProcessingErrorCode::ProfileCreationFailed));
    }

    cmsUInt32Number profileSize = 0;
    if (cmsSaveProfileToMem(profile.get(), nullptr, &profileSize) == 0 || profileSize == 0)
    {
        return IccProfileDataResult::failure(makeError(ColorProcessingErrorCode::ProfileSerializationFailed));
    }

    IccProfileData profileData(profileSize);
    if (cmsSaveProfileToMem(profile.get(), profileData.data(), &profileSize) == 0)
    {
        return IccProfileDataResult::failure(makeError(ColorProcessingErrorCode::ProfileSerializationFailed));
    }
    profileData.resize(profileSize);
    return IccProfileDataResult::success(std::move(profileData));
}

// 목적: caller-owned RGBA8/sRGB source를 ICC profile 사이에서 별도 destination으로 변환
// 입력: source/destination: non-overlapping validated view, sourceProfile/destinationProfile: ICC profile byte data
// 출력: destination 갱신 성공 또는 neutral processing 오류
ColorProcessingResult transformIccRgba8(image::ConstRgba8ImageView source,
                                        image::MutableRgba8ImageView destination,
                                        std::span<const std::uint8_t> sourceProfile,
                                        std::span<const std::uint8_t> destinationProfile)
{
    if (source.width() != destination.width() || source.height() != destination.height())
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::GeometryMismatch));
    }
    if (rangesOverlap(source.bytes(), destination.bytes()))
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::AliasedStorage));
    }
    if (sourceProfile.empty() || destinationProfile.empty())
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::EmptyProfile));
    }

    constexpr std::size_t MaximumProfileSize = std::numeric_limits<cmsUInt32Number>::max();
    if (sourceProfile.size() > MaximumProfileSize || destinationProfile.size() > MaximumProfileSize)
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::ProfileTooLarge));
    }

    const std::uint64_t pixelCount =
        static_cast<std::uint64_t>(source.width()) * static_cast<std::uint64_t>(source.height());
    if (pixelCount > std::numeric_limits<cmsUInt32Number>::max())
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::UnsupportedGeometry));
    }

    const ProfileHandle sourceProfileHandle(
        cmsOpenProfileFromMem(sourceProfile.data(), static_cast<cmsUInt32Number>(sourceProfile.size())));
    if (sourceProfileHandle == nullptr)
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::InvalidSourceProfile));
    }

    const ProfileHandle destinationProfileHandle(
        cmsOpenProfileFromMem(destinationProfile.data(), static_cast<cmsUInt32Number>(destinationProfile.size())));
    if (destinationProfileHandle == nullptr)
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::InvalidDestinationProfile));
    }

    const TransformHandle transform(cmsCreateTransform(sourceProfileHandle.get(),
                                                       TYPE_RGBA_8,
                                                       destinationProfileHandle.get(),
                                                       TYPE_RGBA_8,
                                                       INTENT_PERCEPTUAL,
                                                       cmsFLAGS_COPY_ALPHA));
    if (transform == nullptr)
    {
        return ColorProcessingResult::failure(makeError(ColorProcessingErrorCode::TransformCreationFailed));
    }

    const std::size_t logicalRowBytes = static_cast<std::size_t>(source.width()) * Rgba8BytesPerPixel;
    if (source.rowStride() == logicalRowBytes && destination.rowStride() == logicalRowBytes)
    {
        cmsDoTransform(transform.get(),
                       source.bytes().data(),
                       destination.bytes().data(),
                       static_cast<cmsUInt32Number>(pixelCount));
        return ColorProcessingResult::success({});
    }

    for (std::uint32_t row = 0; row < source.height(); ++row)
    {
        cmsDoTransform(transform.get(), source.rowBytes(row).data(), destination.rowBytes(row).data(), source.width());
    }
    return ColorProcessingResult::success({});
}

}  // namespace flexraw::core::color

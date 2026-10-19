#pragma once

#include <cstdint>
#include <span>
#include <variant>
#include <vector>

#include "result.h"
#include "rgba8_image_view.h"

namespace flexraw::core::color
{

enum class ColorProcessingErrorCode : std::uint8_t
{
    ProfileCreationFailed,
    ProfileSerializationFailed,
    EmptyProfile,
    ProfileTooLarge,
    InvalidSourceProfile,
    InvalidDestinationProfile,
    TransformCreationFailed,
    GeometryMismatch,
    AliasedStorage,
    UnsupportedGeometry,
};

struct ColorProcessingError
{
    ColorProcessingErrorCode code{ColorProcessingErrorCode::ProfileCreationFailed};

    [[nodiscard]] bool operator==(const ColorProcessingError& other) const = default;
};

using IccProfileData = std::vector<std::uint8_t>;
using IccProfileDataResult = types::Result<IccProfileData, ColorProcessingError>;
using ColorProcessingResult = types::Result<std::monostate, ColorProcessingError>;

// 목적: LittleCMS 표준 sRGB ICC profile을 project-owned byte storage로 직렬화
// 입력: 없음
// 출력: ICC profile byte data 또는 neutral processing 오류, 메모리 부족은 예외
[[nodiscard]] IccProfileDataResult createSrgbIccProfileData();

// 목적: caller-owned RGBA8/sRGB source를 ICC profile 사이에서 별도 destination으로 변환
// 입력: source/destination: non-overlapping validated view, sourceProfile/destinationProfile: ICC profile byte data
// 출력: destination 갱신 성공 또는 neutral processing 오류
[[nodiscard]] ColorProcessingResult transformIccRgba8(image::ConstRgba8ImageView source,
                                                      image::MutableRgba8ImageView destination,
                                                      std::span<const std::uint8_t> sourceProfile,
                                                      std::span<const std::uint8_t> destinationProfile);

}  // namespace flexraw::core::color

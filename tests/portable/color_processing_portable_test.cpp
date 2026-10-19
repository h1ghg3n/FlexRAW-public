#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "color_processing.h"

namespace flexraw::core::color
{
namespace
{

constexpr std::uint8_t PaddingSentinel = 0xD7;

// 목적: logical RGBA pixel과 padding을 구분할 deterministic portable test storage 생성
// 입력: width/height/rowStride: image geometry와 row byte 간격
// 출력: logical pixel은 pattern, padding은 sentinel로 채운 byte storage
[[nodiscard]] std::vector<std::uint8_t> makeColorTestStorage(std::uint32_t width,
                                                             std::uint32_t height,
                                                             std::size_t rowStride)
{
    std::vector<std::uint8_t> storage(rowStride * height, PaddingSentinel);
    for (std::uint32_t row = 0; row < height; ++row)
    {
        for (std::uint32_t column = 0; column < width; ++column)
        {
            const std::size_t offset = static_cast<std::size_t>(row) * rowStride + column * 4;
            storage[offset] = static_cast<std::uint8_t>((row * 31 + column * 17 + 19) % 256);
            storage[offset + 1] = static_cast<std::uint8_t>((row * 13 + column * 47 + 53) % 256);
            storage[offset + 2] = static_cast<std::uint8_t>((row * 59 + column * 7 + 101) % 256);
            storage[offset + 3] = static_cast<std::uint8_t>((row * 23 + column * 29 + 127) % 256);
        }
    }
    return storage;
}

TEST(ColorProcessingPortableTest, PreservesLogicalPixelsAndPaddingForSrgbIdentity)
{
    constexpr std::uint32_t Width = 3;
    constexpr std::uint32_t Height = 2;
    constexpr std::size_t SourceStride = 16;
    constexpr std::size_t DestinationStride = 20;

    const IccProfileDataResult profile = createSrgbIccProfileData();
    ASSERT_TRUE(profile.hasValue());

    const std::vector<std::uint8_t> sourceStorage = makeColorTestStorage(Width, Height, SourceStride);
    std::vector<std::uint8_t> destinationStorage(DestinationStride * Height, PaddingSentinel);
    const std::optional<image::ConstRgba8ImageView> source =
        image::ConstRgba8ImageView::create(Width, Height, SourceStride, sourceStorage);
    const std::optional<image::MutableRgba8ImageView> destination =
        image::MutableRgba8ImageView::create(Width, Height, DestinationStride, destinationStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(destination.has_value());

    const ColorProcessingResult result = transformIccRgba8(*source, *destination, profile.value(), profile.value());

    ASSERT_TRUE(result.hasValue());
    for (std::uint32_t row = 0; row < Height; ++row)
    {
        EXPECT_TRUE(std::ranges::equal(source->rowBytes(row), destination->rowBytes(row)));
        const std::size_t paddingBegin = static_cast<std::size_t>(row) * DestinationStride + Width * 4;
        const std::size_t paddingEnd = static_cast<std::size_t>(row + 1) * DestinationStride;
        EXPECT_TRUE(std::all_of(destinationStorage.begin() + static_cast<std::ptrdiff_t>(paddingBegin),
                                destinationStorage.begin() + static_cast<std::ptrdiff_t>(paddingEnd),
                                [](std::uint8_t value) { return value == PaddingSentinel; }));
    }
}

TEST(ColorProcessingPortableTest, RejectsInvalidProfileAndStorageContracts)
{
    const IccProfileDataResult profile = createSrgbIccProfileData();
    ASSERT_TRUE(profile.hasValue());

    std::vector<std::uint8_t> sourceStorage(16, 31);
    std::vector<std::uint8_t> destinationStorage(16, 0);
    const std::optional<image::ConstRgba8ImageView> source = image::ConstRgba8ImageView::create(2, 2, 8, sourceStorage);
    const std::optional<image::MutableRgba8ImageView> destination =
        image::MutableRgba8ImageView::create(2, 2, 8, destinationStorage);
    const std::optional<image::MutableRgba8ImageView> mismatched =
        image::MutableRgba8ImageView::create(1, 2, 4, destinationStorage);
    const std::optional<image::MutableRgba8ImageView> overlapping =
        image::MutableRgba8ImageView::create(2, 2, 8, sourceStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(destination.has_value());
    ASSERT_TRUE(mismatched.has_value());
    ASSERT_TRUE(overlapping.has_value());

    const ColorProcessingResult emptyProfile = transformIccRgba8(*source, *destination, {}, profile.value());
    const std::vector<std::uint8_t> invalidProfile{1, 2, 3, 4};
    const ColorProcessingResult invalidSource =
        transformIccRgba8(*source, *destination, invalidProfile, profile.value());
    const ColorProcessingResult geometryMismatch =
        transformIccRgba8(*source, *mismatched, profile.value(), profile.value());
    const ColorProcessingResult aliased = transformIccRgba8(*source, *overlapping, profile.value(), profile.value());

    ASSERT_TRUE(emptyProfile.hasError());
    EXPECT_EQ(emptyProfile.error().code, ColorProcessingErrorCode::EmptyProfile);
    ASSERT_TRUE(invalidSource.hasError());
    EXPECT_EQ(invalidSource.error().code, ColorProcessingErrorCode::InvalidSourceProfile);
    ASSERT_TRUE(geometryMismatch.hasError());
    EXPECT_EQ(geometryMismatch.error().code, ColorProcessingErrorCode::GeometryMismatch);
    ASSERT_TRUE(aliased.hasError());
    EXPECT_EQ(aliased.error().code, ColorProcessingErrorCode::AliasedStorage);
}

}  // namespace
}  // namespace flexraw::core::color

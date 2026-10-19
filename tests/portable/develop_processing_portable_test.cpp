#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "develop_processing.h"
#include "develop_processing_detail.h"

namespace flexraw::core::develop
{
namespace
{

using image::ConstRgba8ImageView;
using image::MutableRgba8ImageView;

// 목적: test reference에서 linear-light 값을 normalized sRGB로 변환
// 입력: linearValue — 변환할 linear-light 값
// 출력: IEC 61966-2-1 transfer function을 적용한 normalized 값
[[nodiscard]] float referenceLinearToSrgb(float linearValue)
{
    return linearValue <= 0.0031308F ? linearValue * 12.92F : 1.055F * std::pow(linearValue, 1.0F / 2.4F) - 0.055F;
}

// 목적: 최적화 이전 production과 같은 float 연산 순서로 8-bit channel 계산
// 입력: linearValue — 변환할 finite linear-light 값
// 출력: clamp 후 0.5F 더하기와 정수 cast를 적용한 0~255 channel 값
[[nodiscard]] int referenceScalarLinearToSrgbChannel(float linearValue)
{
    const float encodedValue = std::clamp(referenceLinearToSrgb(linearValue), 0.0F, 1.0F);
    return static_cast<int>(encodedValue * 255.0F + 0.5F);
}

// 목적: 독립 reference 식이 지정 channel로 처음 전이하는 float 입력 탐색
// 입력: channel — 전이 뒤의 1~255 sRGB channel 값
// 출력: reference projection이 channel 이상을 반환하는 최소 [0, 1] float 값
[[nodiscard]] float findReferenceLinearToSrgbTransition(int channel)
{
    std::uint32_t lowerBits = std::bit_cast<std::uint32_t>(0.0F);
    std::uint32_t upperBits = std::bit_cast<std::uint32_t>(1.0F);
    while (lowerBits < upperBits)
    {
        const std::uint32_t middleBits = lowerBits + (upperBits - lowerBits) / 2;
        if (referenceScalarLinearToSrgbChannel(std::bit_cast<float>(middleBits)) >= channel)
        {
            upperBits = middleBits;
        }
        else
        {
            lowerBits = middleBits + 1;
        }
    }
    return std::bit_cast<float>(lowerBits);
}

// 목적: Qt 없이 neutral Develop processing을 검증할 deterministic RGBA8 storage 생성
// 입력: width/height/rowStride — image geometry와 row byte 간격
// 출력: logical pixel과 padding을 포함한 caller-owned byte storage
[[nodiscard]] std::vector<std::uint8_t> makePortableTestStorage(std::uint32_t width,
                                                                std::uint32_t height,
                                                                std::size_t rowStride)
{
    std::vector<std::uint8_t> storage(rowStride * height, 0xA5);
    for (std::uint32_t row = 0; row < height; ++row)
    {
        for (std::uint32_t column = 0; column < width; ++column)
        {
            const std::size_t pixelOffset = static_cast<std::size_t>(row) * rowStride + column * 4;
            storage[pixelOffset] = static_cast<std::uint8_t>((row * 31 + column * 17 + 23) % 256);
            storage[pixelOffset + 1] = static_cast<std::uint8_t>((row * 13 + column * 47 + 61) % 256);
            storage[pixelOffset + 2] = static_cast<std::uint8_t>((row * 53 + column * 7 + 101) % 256);
            storage[pixelOffset + 3] = static_cast<std::uint8_t>((row * 19 + column * 29 + 127) % 256);
        }
    }
    return storage;
}

// 목적: point-local과 전체 neighborhood chain을 함께 활성화한 대표 parameter 생성
// 입력: 없음
// 출력: NR, Clarity, Sharpening을 포함한 valid DevelopParams
[[nodiscard]] types::DevelopParams makeCombinedStageParams()
{
    types::DevelopParams params;
    params.exposureEv = 0.25F;
    params.contrast = 0.15F;
    params.saturation = 0.2F;
    params.luminanceNoiseReduction = 0.35F;
    params.colorNoiseReduction = 0.25F;
    params.clarity = 0.4F;
    params.sharpeningAmount = 0.5F;
    params.sharpeningRadius = 2.0F;
    params.sharpeningDetail = 0.3F;
    params.sharpeningMasking = 0.2F;
    return params;
}

TEST(PortableDevelopProcessingTest, CopiesNoOpLogicalPixelsWithoutChangingPadding)
{
    constexpr std::uint32_t Width = 3;
    constexpr std::uint32_t Height = 2;
    constexpr std::size_t SourceStride = 16;
    constexpr std::size_t DestinationStride = 20;
    constexpr std::uint8_t PaddingSentinel = 0x5A;
    const std::vector<std::uint8_t> sourceStorage = makePortableTestStorage(Width, Height, SourceStride);
    std::vector<std::uint8_t> destinationStorage(DestinationStride * Height, PaddingSentinel);

    const std::optional<ConstRgba8ImageView> source =
        ConstRgba8ImageView::create(Width, Height, SourceStride, sourceStorage);
    const std::optional<MutableRgba8ImageView> destination =
        MutableRgba8ImageView::create(Width, Height, DestinationStride, destinationStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(destination.has_value());

    const DevelopProcessingResult result = applyDevelop(*source, *destination, types::DevelopParams{}, 1);

    ASSERT_TRUE(result.hasValue());
    for (std::uint32_t row = 0; row < Height; ++row)
    {
        EXPECT_TRUE(
            std::equal(source->rowBytes(row).begin(), source->rowBytes(row).end(), destination->rowBytes(row).begin()));
        const std::size_t paddingBegin = static_cast<std::size_t>(row) * DestinationStride + Width * 4;
        const std::size_t paddingEnd = static_cast<std::size_t>(row + 1) * DestinationStride;
        EXPECT_TRUE(std::all_of(destinationStorage.begin() + static_cast<std::ptrdiff_t>(paddingBegin),
                                destinationStorage.begin() + static_cast<std::ptrdiff_t>(paddingEnd),
                                [=](std::uint8_t value) { return value == PaddingSentinel; }));
    }
}

TEST(PortableDevelopProcessingTest, KeepsCombinedStageOutputEquivalentAcrossGrantedPermitBounds)
{
    constexpr std::uint32_t Width = 9;
    constexpr std::uint32_t Height = 129;
    constexpr std::size_t RowStride = Width * 4;
    const std::vector<std::uint8_t> sourceStorage = makePortableTestStorage(Width, Height, RowStride);
    std::vector<std::uint8_t> serialStorage(RowStride * Height);
    std::vector<std::uint8_t> boundedStorage(RowStride * Height);
    const std::optional<ConstRgba8ImageView> source =
        ConstRgba8ImageView::create(Width, Height, RowStride, sourceStorage);
    const std::optional<MutableRgba8ImageView> serial =
        MutableRgba8ImageView::create(Width, Height, RowStride, serialStorage);
    const std::optional<MutableRgba8ImageView> bounded =
        MutableRgba8ImageView::create(Width, Height, RowStride, boundedStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(serial.has_value());
    ASSERT_TRUE(bounded.has_value());
    const types::DevelopParams params = makeCombinedStageParams();

    const DevelopProcessingResult serialResult = applyDevelop(*source, *serial, params, 1);
    const DevelopProcessingResult boundedResult = applyDevelop(*source, *bounded, params, 4);

    ASSERT_TRUE(serialResult.hasValue());
    ASSERT_TRUE(boundedResult.hasValue());
    EXPECT_EQ(serialStorage, boundedStorage);
    EXPECT_NE(sourceStorage, boundedStorage);
    for (std::uint32_t row = 0; row < Height; ++row)
    {
        for (std::uint32_t column = 0; column < Width; ++column)
        {
            const std::size_t alphaOffset = static_cast<std::size_t>(row) * RowStride + column * 4 + 3;
            EXPECT_EQ(sourceStorage[alphaOffset], boundedStorage[alphaOffset]);
        }
    }
}

// 목적: radius-one Sharpening specialization의 stripe boundary와 permit 동등성 검증
// 입력: 64-row boundary를 넘는 RGBA8 source와 c1/c4 execution grant
// 출력: c1/c4 RGB·alpha 결과가 byte-exact하면 성공
TEST(PortableDevelopProcessingTest, KeepsRadiusOneSharpeningEquivalentAcrossGrantedPermitBounds)
{
    constexpr std::uint32_t Width = 9;
    constexpr std::uint32_t Height = 129;
    constexpr std::size_t RowStride = Width * 4;
    const std::vector<std::uint8_t> sourceStorage = makePortableTestStorage(Width, Height, RowStride);
    std::vector<std::uint8_t> serialStorage(RowStride * Height);
    std::vector<std::uint8_t> boundedStorage(RowStride * Height);
    const std::optional<ConstRgba8ImageView> source =
        ConstRgba8ImageView::create(Width, Height, RowStride, sourceStorage);
    const std::optional<MutableRgba8ImageView> serial =
        MutableRgba8ImageView::create(Width, Height, RowStride, serialStorage);
    const std::optional<MutableRgba8ImageView> bounded =
        MutableRgba8ImageView::create(Width, Height, RowStride, boundedStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(serial.has_value());
    ASSERT_TRUE(bounded.has_value());
    types::DevelopParams params;
    params.sharpeningAmount = 0.5F;
    params.sharpeningRadius = 1.0F;
    params.sharpeningDetail = 0.3F;
    params.sharpeningMasking = 0.2F;

    const DevelopProcessingResult serialResult = applyDevelop(*source, *serial, params, 1);
    const DevelopProcessingResult boundedResult = applyDevelop(*source, *bounded, params, 4);

    ASSERT_TRUE(serialResult.hasValue());
    ASSERT_TRUE(boundedResult.hasValue());
    EXPECT_EQ(serialStorage, boundedStorage);
    EXPECT_NE(sourceStorage, boundedStorage);
}

TEST(PortableDevelopProcessingTest, PreservesNeutralValidationErrors)
{
    std::vector<std::uint8_t> sourceStorage(4, 0);
    std::vector<std::uint8_t> destinationStorage(4, 0);
    const std::optional<ConstRgba8ImageView> source = ConstRgba8ImageView::create(1, 1, 4, sourceStorage);
    const std::optional<MutableRgba8ImageView> destination = MutableRgba8ImageView::create(1, 1, 4, destinationStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(destination.has_value());

    types::DevelopParams invalidParams;
    invalidParams.exposureEv = std::numeric_limits<float>::infinity();
    const DevelopProcessingResult invalidParamsResult = applyDevelop(*source, *destination, invalidParams, 1);
    const DevelopProcessingResult invalidGrantResult = applyDevelop(*source, *destination, types::DevelopParams{}, 0);

    ASSERT_TRUE(invalidParamsResult.hasError());
    ASSERT_TRUE(invalidGrantResult.hasError());
    EXPECT_EQ(DevelopProcessingErrorCode::InvalidParameters, invalidParamsResult.error().code);
    EXPECT_EQ(DevelopProcessingErrorCode::InvalidExecutionGrant, invalidGrantResult.error().code);
}

TEST(PortableDevelopProcessingTest, MatchesLinearToSrgbReferenceAtEveryTransitionAndAdjacentFloat)
{
    constexpr int ChannelValueCount = 256;
    for (int channel = 1; channel < ChannelValueCount; ++channel)
    {
        const float transition = findReferenceLinearToSrgbTransition(channel);
        const std::array<float, 3> samples{
            std::nextafter(transition, 0.0F),
            transition,
            std::nextafter(transition, 1.0F),
        };
        for (float sample : samples)
        {
            SCOPED_TRACE(::testing::Message() << "channel=" << channel << " sample=" << sample);
            EXPECT_EQ(referenceScalarLinearToSrgbChannel(sample), detail::projectLinearToSrgbChannel(sample));
        }
    }
}

TEST(PortableDevelopProcessingTest, MatchesLinearToSrgbReferenceAtEveryLookupBucketEdge)
{
    constexpr std::size_t LookupSize = 65536;
    constexpr float LookupScale = static_cast<float>(LookupSize - 1);
    for (std::size_t index = 0; index < LookupSize; ++index)
    {
        const float edge = static_cast<float>(index) / LookupScale;
        SCOPED_TRACE(::testing::Message() << "bucket=" << index);
        EXPECT_EQ(referenceScalarLinearToSrgbChannel(edge), detail::projectLinearToSrgbChannel(edge));
        if (index > 0)
        {
            const float before = std::nextafter(edge, 0.0F);
            EXPECT_EQ(referenceScalarLinearToSrgbChannel(before), detail::projectLinearToSrgbChannel(before));
        }
        if (index + 1 < LookupSize)
        {
            const float after = std::nextafter(edge, 1.0F);
            EXPECT_EQ(referenceScalarLinearToSrgbChannel(after), detail::projectLinearToSrgbChannel(after));
        }
    }
}

TEST(PortableDevelopProcessingTest, MatchesLinearToSrgbReferenceAtFiniteClampEndpoints)
{
    const std::array<float, 10> samples{
        std::numeric_limits<float>::lowest(),
        -1.0F,
        -std::numeric_limits<float>::denorm_min(),
        -0.0F,
        0.0F,
        std::numeric_limits<float>::denorm_min(),
        std::nextafter(1.0F, 0.0F),
        1.0F,
        std::nextafter(1.0F, std::numeric_limits<float>::max()),
        std::numeric_limits<float>::max(),
    };
    for (float sample : samples)
    {
        SCOPED_TRACE(::testing::Message() << "sample=" << sample);
        EXPECT_EQ(referenceScalarLinearToSrgbChannel(sample), detail::projectLinearToSrgbChannel(sample));
    }
}

}  // namespace
}  // namespace flexraw::core::develop

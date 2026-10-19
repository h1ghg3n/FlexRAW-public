#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <QImage>

#include <gtest/gtest.h>

#include "develop.h"
#include "develop_processing.h"
#include "develop_qt_adapter.h"

namespace flexraw::core::develop
{
namespace
{

using image::ConstRgba8ImageView;
using image::MutableRgba8ImageView;

// 목적: RGBA channel과 alpha가 위치마다 다른 deterministic test image 생성
// 입력: width/height: 생성할 image geometry
// 출력: top-down RGBA8888 test image
[[nodiscard]] QImage makeDevelopProcessingTestImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGBA8888);
    for (int row = 0; row < height; ++row)
    {
        std::uint8_t* pixels = reinterpret_cast<std::uint8_t*>(image.scanLine(row));
        for (int column = 0; column < width; ++column)
        {
            std::uint8_t* pixel = pixels + column * 4;
            pixel[0] = static_cast<std::uint8_t>((row * 31 + column * 17 + 23) % 256);
            pixel[1] = static_cast<std::uint8_t>((row * 13 + column * 47 + 61) % 256);
            pixel[2] = static_cast<std::uint8_t>((row * 53 + column * 7 + 101) % 256);
            pixel[3] = static_cast<std::uint8_t>((row * 19 + column * 29 + 127) % 256);
        }
    }
    return image;
}

// 목적: QImage RGBA storage를 validated immutable view로 투영
// 입력: image: RGBA8888 test image
// 출력: QImage lifetime에 묶인 validated view
[[nodiscard]] std::optional<ConstRgba8ImageView> makeConstView(const QImage& image)
{
    return ConstRgba8ImageView::create(
        static_cast<std::uint32_t>(image.width()),
        static_cast<std::uint32_t>(image.height()),
        static_cast<std::size_t>(image.bytesPerLine()),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(image.constBits()),
                                      static_cast<std::size_t>(image.sizeInBytes())));
}

// 목적: QImage 결과와 caller-owned tight RGBA storage의 logical pixel을 비교
// 입력: expected: 비교 기준 image, actual: width * 4 stride의 byte storage
// 출력: 모든 logical pixel byte에 대한 Google Test assertion
void expectImageMatchesStorage(const QImage& expected, std::span<const std::uint8_t> actual)
{
    const std::size_t rowSize = static_cast<std::size_t>(expected.width()) * 4;
    ASSERT_EQ(rowSize * static_cast<std::size_t>(expected.height()), actual.size());
    for (int row = 0; row < expected.height(); ++row)
    {
        const std::uint8_t* expectedRow = reinterpret_cast<const std::uint8_t*>(expected.constScanLine(row));
        const std::uint8_t* actualRow = actual.data() + static_cast<std::size_t>(row) * rowSize;
        EXPECT_TRUE(std::equal(expectedRow, expectedRow + rowSize, actualRow)) << "row=" << row;
    }
}

// 목적: QImage와 stride가 다른 neutral destination의 logical RGBA pixel만 비교
// 입력: expected: 비교 기준 image, actual: padding을 포함할 수 있는 destination view
// 출력: width * 4 logical byte에 대한 Google Test assertion
void expectImageMatchesView(const QImage& expected, MutableRgba8ImageView actual)
{
    ASSERT_EQ(static_cast<std::uint32_t>(expected.width()), actual.width());
    ASSERT_EQ(static_cast<std::uint32_t>(expected.height()), actual.height());
    for (int row = 0; row < expected.height(); ++row)
    {
        const std::uint8_t* expectedRow = reinterpret_cast<const std::uint8_t*>(expected.constScanLine(row));
        const std::span<const std::uint8_t> actualRow = actual.rowBytes(static_cast<std::uint32_t>(row));
        EXPECT_TRUE(std::equal(expectedRow, expectedRow + actualRow.size(), actualRow.begin())) << "row=" << row;
    }
}

// 목적: adapter/neutral semantic matrix의 point-local 대표 parameter 생성
// 입력: 없음
// 출력: neighborhood stage를 제외한 대표 DevelopParams
[[nodiscard]] types::DevelopParams makePointLocalAcceptanceParams()
{
    types::DevelopParams params;
    params.exposureEv = 0.35F;
    params.contrast = 0.2F;
    params.highlights = -0.15F;
    params.shadows = 0.25F;
    params.whites = 0.1F;
    params.blacks = -0.2F;
    params.saturation = 0.3F;
    params.vibrance = 0.4F;
    params.hsl.channels[1] = {0.3F, -0.2F, 0.4F};
    params.whiteBalanceMode = types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 7200.0F;
    params.whiteBalanceTint = -0.15F;
    params.dehaze = 0.2F;
    params.toneCurveDarks = -0.1F;
    params.pointCurveHighlights = 0.15F;
    return params;
}

// 목적: adapter/neutral semantic matrix의 optional spatial stage parameter 생성
// 입력: noiseReduction/clarity/sharpening: 활성화할 stage 조합
// 출력: 선택된 stage만 활성화한 DevelopParams
[[nodiscard]] types::DevelopParams makeSpatialAcceptanceParams(bool noiseReduction, bool clarity, bool sharpening)
{
    types::DevelopParams params;
    if (noiseReduction)
    {
        params.luminanceNoiseReduction = 0.35F;
        params.colorNoiseReduction = 0.25F;
    }
    if (clarity)
    {
        params.clarity = 0.4F;
    }
    if (sharpening)
    {
        params.sharpeningAmount = 0.5F;
        params.sharpeningRadius = 2.0F;
        params.sharpeningDetail = 0.3F;
        params.sharpeningMasking = 0.2F;
    }
    return params;
}

TEST(DevelopProcessingTest, CopiesNoOpPixelsAcrossDifferentStridesWithoutTouchingPadding)
{
    constexpr std::uint32_t Width = 2;
    constexpr std::uint32_t Height = 2;
    constexpr std::size_t SourceStride = 12;
    constexpr std::size_t DestinationStride = 16;
    constexpr std::uint8_t PaddingSentinel = 0xA5;
    std::vector<std::uint8_t> sourceStorage(SourceStride * Height, 0x3C);
    std::vector<std::uint8_t> destinationStorage(DestinationStride * Height, PaddingSentinel);
    for (std::uint32_t row = 0; row < Height; ++row)
    {
        for (std::size_t byte = 0; byte < Width * 4; ++byte)
        {
            sourceStorage[static_cast<std::size_t>(row) * SourceStride + byte] =
                static_cast<std::uint8_t>(row * 32 + byte);
        }
    }

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

TEST(DevelopProcessingTest, MatchesQtCompatibilityPathForPointLocalStages)
{
    const QImage sourceImage = makeDevelopProcessingTestImage(7, 5);
    types::DevelopParams params;
    params.exposureEv = 0.35F;
    params.contrast = 0.2F;
    params.highlights = -0.15F;
    params.shadows = 0.25F;
    params.whites = 0.1F;
    params.blacks = -0.2F;
    params.saturation = 0.3F;
    params.vibrance = 0.4F;
    params.hsl.channels[1] = {0.3F, -0.2F, 0.4F};
    params.whiteBalanceMode = types::WhiteBalanceMode::Custom;
    params.whiteBalanceTemperatureKelvin = 7200.0F;
    params.whiteBalanceTint = -0.15F;
    params.dehaze = 0.2F;
    params.toneCurveDarks = -0.1F;
    params.pointCurveHighlights = 0.15F;
    const DevelopImageResult expected = qt_adapter::apply(sourceImage, params, 1);
    ASSERT_TRUE(expected.hasValue());

    const std::size_t rowStride = static_cast<std::size_t>(sourceImage.width()) * 4;
    std::vector<std::uint8_t> destinationStorage(rowStride * static_cast<std::size_t>(sourceImage.height()));
    const std::optional<ConstRgba8ImageView> source = makeConstView(sourceImage);
    const std::optional<MutableRgba8ImageView> destination =
        MutableRgba8ImageView::create(static_cast<std::uint32_t>(sourceImage.width()),
                                      static_cast<std::uint32_t>(sourceImage.height()),
                                      rowStride,
                                      destinationStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(destination.has_value());

    const DevelopProcessingResult result = applyDevelop(*source, *destination, params, 1);

    ASSERT_TRUE(result.hasValue());
    expectImageMatchesStorage(expected.value(), destinationStorage);
}

TEST(DevelopProcessingTest, MatchesQtCompatibilityPathForBoundedNeighborhoodStages)
{
    const QImage sourceImage = makeDevelopProcessingTestImage(11, 9);
    types::DevelopParams params;
    params.exposureEv = 0.2F;
    params.luminanceNoiseReduction = 0.35F;
    params.colorNoiseReduction = 0.25F;
    params.clarity = 0.4F;
    params.sharpeningAmount = 0.5F;
    params.sharpeningRadius = 2.0F;
    params.sharpeningDetail = 0.3F;
    params.sharpeningMasking = 0.2F;
    const DevelopImageResult expected = qt_adapter::apply(sourceImage, params, 4);
    ASSERT_TRUE(expected.hasValue());

    const std::size_t rowStride = static_cast<std::size_t>(sourceImage.width()) * 4;
    std::vector<std::uint8_t> destinationStorage(rowStride * static_cast<std::size_t>(sourceImage.height()));
    const std::optional<ConstRgba8ImageView> source = makeConstView(sourceImage);
    const std::optional<MutableRgba8ImageView> destination =
        MutableRgba8ImageView::create(static_cast<std::uint32_t>(sourceImage.width()),
                                      static_cast<std::uint32_t>(sourceImage.height()),
                                      rowStride,
                                      destinationStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(destination.has_value());

    const DevelopProcessingResult result = applyDevelop(*source, *destination, params, 4);

    ASSERT_TRUE(result.hasValue());
    expectImageMatchesStorage(expected.value(), destinationStorage);
}

TEST(DevelopProcessingTest, MatchesQtCompatibilityPathAcrossOptionalStageMatrix)
{
    struct AcceptanceCase
    {
        std::string_view name;
        types::DevelopParams params;
    };

    std::array<AcceptanceCase, 8> cases{{
        {"point-local", makePointLocalAcceptanceParams()},
        {"noise-only", makeSpatialAcceptanceParams(true, false, false)},
        {"clarity-only", makeSpatialAcceptanceParams(false, true, false)},
        {"sharpening-radius-one", makeSpatialAcceptanceParams(false, false, true)},
        {"sharpening-only", makeSpatialAcceptanceParams(false, false, true)},
        {"noise-clarity", makeSpatialAcceptanceParams(true, true, false)},
        {"clarity-sharpening", makeSpatialAcceptanceParams(false, true, true)},
        {"point-local-noise-clarity-sharpening", makePointLocalAcceptanceParams()},
    }};
    cases[3].params.sharpeningRadius = 1.0F;
    cases.back().params.luminanceNoiseReduction = 0.35F;
    cases.back().params.colorNoiseReduction = 0.25F;
    cases.back().params.clarity = 0.4F;
    cases.back().params.sharpeningAmount = 0.5F;
    cases.back().params.sharpeningRadius = 3.0F;
    cases.back().params.sharpeningDetail = 0.3F;
    cases.back().params.sharpeningMasking = 0.2F;

    const QImage sourceImage = makeDevelopProcessingTestImage(13, 11);
    const std::optional<ConstRgba8ImageView> source = makeConstView(sourceImage);
    ASSERT_TRUE(source.has_value());
    constexpr std::uint8_t PaddingSentinel = 0xA5;
    const std::size_t logicalRowBytes = static_cast<std::size_t>(sourceImage.width()) * 4;
    const std::size_t destinationStride = logicalRowBytes + 12;

    for (const AcceptanceCase& testCase : cases)
    {
        SCOPED_TRACE(testCase.name);
        const DevelopImageResult expected = qt_adapter::apply(sourceImage, testCase.params, 4);
        ASSERT_TRUE(expected.hasValue());

        std::vector<std::uint8_t> destinationStorage(destinationStride * static_cast<std::size_t>(sourceImage.height()),
                                                     PaddingSentinel);
        const std::optional<MutableRgba8ImageView> destination =
            MutableRgba8ImageView::create(static_cast<std::uint32_t>(sourceImage.width()),
                                          static_cast<std::uint32_t>(sourceImage.height()),
                                          destinationStride,
                                          destinationStorage);
        ASSERT_TRUE(destination.has_value());

        const DevelopProcessingResult result = applyDevelop(*source, *destination, testCase.params, 4);

        ASSERT_TRUE(result.hasValue());
        expectImageMatchesView(expected.value(), *destination);
        for (int row = 0; row < sourceImage.height(); ++row)
        {
            const std::size_t paddingBegin = static_cast<std::size_t>(row) * destinationStride + logicalRowBytes;
            const std::size_t paddingEnd = static_cast<std::size_t>(row + 1) * destinationStride;
            EXPECT_TRUE(std::all_of(destinationStorage.begin() + static_cast<std::ptrdiff_t>(paddingBegin),
                                    destinationStorage.begin() + static_cast<std::ptrdiff_t>(paddingEnd),
                                    [=](std::uint8_t value) { return value == PaddingSentinel; }));
        }
    }
}

TEST(DevelopProcessingTest, KeepsNoOpResultEquivalentWithBoundarySpecificOwnership)
{
    const QImage sourceImage = makeDevelopProcessingTestImage(7, 5);
    const DevelopImageResult adapterResult = qt_adapter::apply(sourceImage, types::DevelopParams{}, 1);
    ASSERT_TRUE(adapterResult.hasValue());
    EXPECT_EQ(sourceImage, adapterResult.value());
    EXPECT_EQ(sourceImage.constBits(), adapterResult.value().constBits());

    const std::optional<ConstRgba8ImageView> source = makeConstView(sourceImage);
    const std::size_t rowStride = static_cast<std::size_t>(sourceImage.width()) * 4;
    std::vector<std::uint8_t> destinationStorage(rowStride * static_cast<std::size_t>(sourceImage.height()));
    const std::optional<MutableRgba8ImageView> destination =
        MutableRgba8ImageView::create(static_cast<std::uint32_t>(sourceImage.width()),
                                      static_cast<std::uint32_t>(sourceImage.height()),
                                      rowStride,
                                      destinationStorage);
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(destination.has_value());

    const DevelopProcessingResult neutralResult = applyDevelop(*source, *destination, types::DevelopParams{}, 1);

    ASSERT_TRUE(neutralResult.hasValue());
    expectImageMatchesStorage(sourceImage, destinationStorage);
    EXPECT_NE(static_cast<const void*>(sourceImage.constBits()), static_cast<const void*>(destinationStorage.data()));
}

TEST(DevelopProcessingTest, PreservesCompatibilityErrorsThroughLegacyForwarding)
{
    const QImage sourceImage = makeDevelopProcessingTestImage(3, 3);
    types::DevelopParams invalidParams;
    invalidParams.exposureEv = std::numeric_limits<float>::infinity();

    const std::array<std::pair<DevelopImageResult, DevelopImageResult>, 3> cases{{
        {qt_adapter::apply(QImage{}, types::DevelopParams{}, 1), applyDevelop(QImage{}, types::DevelopParams{}, 1)},
        {qt_adapter::apply(sourceImage, invalidParams, 1), applyDevelop(sourceImage, invalidParams, 1)},
        {qt_adapter::apply(sourceImage, types::DevelopParams{}, 0),
         applyDevelop(sourceImage, types::DevelopParams{}, 0)},
    }};

    for (const auto& [adapterResult, legacyResult] : cases)
    {
        ASSERT_TRUE(adapterResult.hasError());
        ASSERT_TRUE(legacyResult.hasError());
        EXPECT_EQ(adapterResult.error().code, legacyResult.error().code);
        EXPECT_EQ(adapterResult.error().message, legacyResult.error().message);
    }
}

TEST(DevelopProcessingTest, RejectsMismatchedGeometryAndOverlappingStorage)
{
    std::vector<std::uint8_t> sourceStorage(24, 0);
    std::vector<std::uint8_t> destinationStorage(16, 0);
    const std::optional<ConstRgba8ImageView> source = ConstRgba8ImageView::create(2, 2, 8, sourceStorage);
    const std::optional<MutableRgba8ImageView> mismatched = MutableRgba8ImageView::create(1, 2, 4, destinationStorage);
    const std::optional<MutableRgba8ImageView> overlapping = MutableRgba8ImageView::create(
        2, 2, 8, std::span<std::uint8_t>(sourceStorage.data() + 4, sourceStorage.size() - 4));
    ASSERT_TRUE(source.has_value());
    ASSERT_TRUE(mismatched.has_value());
    ASSERT_TRUE(overlapping.has_value());

    const DevelopProcessingResult mismatchedResult = applyDevelop(*source, *mismatched, types::DevelopParams{}, 1);
    const DevelopProcessingResult overlappingResult = applyDevelop(*source, *overlapping, types::DevelopParams{}, 1);

    ASSERT_TRUE(mismatchedResult.hasError());
    ASSERT_TRUE(overlappingResult.hasError());
    EXPECT_EQ(DevelopProcessingErrorCode::GeometryMismatch, mismatchedResult.error().code);
    EXPECT_EQ(DevelopProcessingErrorCode::AliasedStorage, overlappingResult.error().code);
}

TEST(DevelopProcessingTest, RejectsInvalidParametersAndExecutionGrant)
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

TEST(DevelopProcessingTest, LegacyQImageEntryForwardsToQtCompatibilityAdapter)
{
    const QImage sourceImage = makeDevelopProcessingTestImage(9, 7);
    types::DevelopParams params;
    params.exposureEv = 0.3F;
    params.clarity = 0.25F;

    const DevelopImageResult adapterResult = qt_adapter::apply(sourceImage, params, 3);
    const DevelopImageResult legacyResult = applyDevelop(sourceImage, params, 3);

    ASSERT_TRUE(adapterResult.hasValue());
    ASSERT_TRUE(legacyResult.hasValue());
    EXPECT_EQ(adapterResult.value(), legacyResult.value());
}

}  // namespace
}  // namespace flexraw::core::develop

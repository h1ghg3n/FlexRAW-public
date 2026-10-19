#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "raw_processing.h"
#include "raw_rgba8_bridge.h"

namespace flexraw::core::raw
{
namespace
{

static_assert(!std::is_copy_constructible_v<RawDecodedRaster8>);
static_assert(!std::is_copy_assignable_v<RawDecodedRaster8>);
static_assert(std::is_nothrow_move_constructible_v<RawDecodedRaster8>);

TEST(RawProcessingPortable, OwnsValidatedRgbStorageAndObservation)
{
    const RawDecodeObservation observation{2, 1, 2, 1, 6};
    std::optional<RawDecodedRaster8> raster = RawDecodedRaster8::create(std::vector<std::uint8_t>{255, 0, 0, 0, 255, 0},
                                                                        2,
                                                                        1,
                                                                        6,
                                                                        RawRasterLayout::Rgb8,
                                                                        RawOrientation::Rotate90Clockwise,
                                                                        observation);

    ASSERT_TRUE(raster.has_value());
    EXPECT_EQ(2U, raster->width());
    EXPECT_EQ(1U, raster->height());
    EXPECT_EQ(6U, raster->rowStride());
    EXPECT_EQ(3U, raster->channelCount());
    EXPECT_EQ(RawRasterLayout::Rgb8, raster->layout());
    EXPECT_EQ(RawOrientation::Rotate90Clockwise, raster->pendingOrientation());
    EXPECT_EQ(observation, raster->observation());
    EXPECT_EQ((std::vector<std::uint8_t>{255, 0, 0, 0, 255, 0}),
              std::vector<std::uint8_t>(raster->bytes().begin(), raster->bytes().end()));
}

TEST(RawProcessingPortable, TransfersStorageToOneDownstreamOwner)
{
    const std::vector<std::uint8_t> expected{255, 0, 0, 0, 255, 0};
    std::optional<RawDecodedRaster8> raster = RawDecodedRaster8::create(
        expected, 2, 1, 6, RawRasterLayout::Rgb8, RawOrientation::AsDecoded, RawDecodeObservation{2, 1, 2, 1, 0});

    ASSERT_TRUE(raster.has_value());
    const std::vector<std::uint8_t> transferred = std::move(*raster).takeBytes();

    EXPECT_EQ(expected, transferred);
}

TEST(RawProcessingPortable, RejectsIncompleteOrMismatchedStorage)
{
    const RawDecodeObservation observation{2, 1, 2, 1, 0};
    const std::optional<RawDecodedRaster8> incomplete = RawDecodedRaster8::create(
        std::vector<std::uint8_t>{255, 0, 0}, 2, 1, 6, RawRasterLayout::Rgb8, RawOrientation::AsDecoded, observation);
    const std::optional<RawDecodedRaster8> mismatched = RawDecodedRaster8::create(std::vector<std::uint8_t>(6),
                                                                                  2,
                                                                                  1,
                                                                                  6,
                                                                                  RawRasterLayout::Rgb8,
                                                                                  RawOrientation::AsDecoded,
                                                                                  RawDecodeObservation{2, 1, 1, 1, 0});
    const std::optional<RawDecodedRaster8> invalidLayout = RawDecodedRaster8::create(std::vector<std::uint8_t>(6),
                                                                                     2,
                                                                                     1,
                                                                                     6,
                                                                                     static_cast<RawRasterLayout>(255),
                                                                                     RawOrientation::AsDecoded,
                                                                                     observation);

    EXPECT_FALSE(incomplete.has_value());
    EXPECT_FALSE(mismatched.has_value());
    EXPECT_FALSE(invalidLayout.has_value());
}

TEST(RawProcessingPortable, MapsSourceFlipWithoutQt)
{
    EXPECT_EQ(RawOrientation::AsDecoded, rawOrientationFromSourceFlip(0));
    EXPECT_EQ(RawOrientation::Rotate180, rawOrientationFromSourceFlip(3));
    EXPECT_EQ(RawOrientation::Rotate90CounterClockwise, rawOrientationFromSourceFlip(5));
    EXPECT_EQ(RawOrientation::Rotate90Clockwise, rawOrientationFromSourceFlip(6));
}

TEST(RawProcessingPortable, RejectsEmptyDecodePath)
{
    const RawDecodeResult result = decodeRawRaster8({});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RawDecodeErrorCode::InvalidPath, result.error().code);
}

// 목적: red channel label을 가진 2x3 RGB fixture 생성
// 입력: orientation: bridge가 적용할 pending orientation
// 출력: label 1~6을 row-major로 가진 validated RAW owner
[[nodiscard]] RawDecodedRaster8 makeLabeledRgbRaster(const RawOrientation orientation)
{
    std::vector<std::uint8_t> bytes;
    for (std::uint8_t label = 1U; label <= 6U; ++label)
    {
        bytes.insert(bytes.end(), {label, static_cast<std::uint8_t>(label + 10U), 200U});
    }
    std::optional<RawDecodedRaster8> raster = RawDecodedRaster8::create(
        std::move(bytes), 2, 3, 6, RawRasterLayout::Rgb8, orientation, RawDecodeObservation{2, 3, 2, 3, 0});
    return std::move(*raster);
}

// 목적: RGBA8 image의 red channel을 row-major label vector로 수집
// 입력: image: bridge output owner
// 출력: logical pixel 순서의 red channel 값
[[nodiscard]] std::vector<std::uint8_t> redLabels(const image::OwnedRgba8Image& image)
{
    std::vector<std::uint8_t> labels;
    const image::ConstRgba8ImageView view = image.constView();
    for (std::uint32_t row = 0; row < view.height(); ++row)
    {
        for (std::uint32_t column = 0; column < view.width(); ++column)
        {
            labels.push_back(view.rowBytes(row)[static_cast<std::size_t>(column) * 4U]);
        }
    }
    return labels;
}

TEST(RawProcessingPortable, ExpandsRgbAndAppliesEveryPendingOrientation)
{
    struct Case
    {
        RawOrientation orientation;
        std::uint32_t width;
        std::uint32_t height;
        std::vector<std::uint8_t> labels;
    };
    const std::vector<Case> cases{
        {RawOrientation::AsDecoded, 2, 3, {1, 2, 3, 4, 5, 6}},
        {RawOrientation::Rotate180, 2, 3, {6, 5, 4, 3, 2, 1}},
        {RawOrientation::Rotate90Clockwise, 3, 2, {5, 3, 1, 6, 4, 2}},
        {RawOrientation::Rotate90CounterClockwise, 3, 2, {2, 4, 6, 1, 3, 5}},
    };

    for (const Case& testCase : cases)
    {
        RawRgba8BridgeResult result = consumeRawRasterToRgba8(makeLabeledRgbRaster(testCase.orientation));
        ASSERT_TRUE(result.hasValue());
        EXPECT_EQ(testCase.width, result.value().width());
        EXPECT_EQ(testCase.height, result.value().height());
        EXPECT_EQ(testCase.labels, redLabels(result.value()));
        for (std::uint32_t row = 0; row < result.value().height(); ++row)
        {
            const std::span<const std::uint8_t> rowBytes = result.value().constView().rowBytes(row);
            for (std::size_t alpha = 3U; alpha < rowBytes.size(); alpha += 4U)
            {
                EXPECT_EQ(255U, rowBytes[alpha]);
            }
        }
    }
}

TEST(RawProcessingPortable, TransfersAsDecodedRgbaStorageWithoutFullFrameCopy)
{
    std::optional<RawDecodedRaster8> raster =
        RawDecodedRaster8::create(std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8},
                                  2,
                                  1,
                                  8,
                                  RawRasterLayout::Rgba8,
                                  RawOrientation::AsDecoded,
                                  RawDecodeObservation{2, 1, 2, 1, 0});
    ASSERT_TRUE(raster.has_value());
    const std::uint8_t* originalStorage = raster->bytes().data();

    RawRgba8BridgeResult result = consumeRawRasterToRgba8(std::move(*raster));

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(originalStorage, result.value().bytes().data());
    EXPECT_EQ((std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8}),
              std::vector<std::uint8_t>(result.value().bytes().begin(), result.value().bytes().end()));
}

}  // namespace
}  // namespace flexraw::core::raw

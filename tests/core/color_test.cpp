#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <QColor>
#include <QColorSpace>

#include <gtest/gtest.h>

#include "color_processing.h"
#include "color_qt_adapter.h"

namespace flexraw::core::color
{
namespace
{

TEST(Color, CreatesSrgbProfile)
{
    const IccProfileResult profileResult = qt_adapter::createSrgbIccProfile();

    ASSERT_TRUE(profileResult.hasValue());
    EXPECT_FALSE(profileResult.value().isEmpty());
}

TEST(Color, CreatesStandardRgbProfiles)
{
    for (const RgbColorSpace colorSpace : {RgbColorSpace::Srgb, RgbColorSpace::AdobeRgb, RgbColorSpace::DisplayP3})
    {
        const IccProfileResult profileResult = qt_adapter::createRgbIccProfile(colorSpace);

        ASSERT_TRUE(profileResult.hasValue());
        EXPECT_TRUE(QColorSpace::fromIccProfile(profileResult.value()).isValid());
    }
}

TEST(Color, PreservesRgbaPixelsForSrgbIdentityTransform)
{
    const IccProfileResult profileResult = qt_adapter::createSrgbIccProfile();
    ASSERT_TRUE(profileResult.hasValue());

    QImage sourceImage(QSize(2, 1), QImage::Format_RGBA8888);
    sourceImage.setPixelColor(0, 0, QColor(16, 128, 240, 77));
    sourceImage.setPixelColor(1, 0, QColor(255, 0, 63, 201));

    const ColorImageResult transformResult =
        qt_adapter::transformIccImage(sourceImage, profileResult.value(), profileResult.value());

    ASSERT_TRUE(transformResult.hasValue());
    EXPECT_EQ(sourceImage.pixelColor(0, 0), transformResult.value().pixelColor(0, 0));
    EXPECT_EQ(sourceImage.pixelColor(1, 0), transformResult.value().pixelColor(1, 0));
}

TEST(Color, RejectsEmptySourceImage)
{
    const IccProfileResult profileResult = qt_adapter::createSrgbIccProfile();
    ASSERT_TRUE(profileResult.hasValue());

    const ColorImageResult transformResult =
        qt_adapter::transformIccImage(QImage(), profileResult.value(), profileResult.value());

    ASSERT_TRUE(transformResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, transformResult.error().code);
}

TEST(Color, RejectsInvalidIccProfile)
{
    const IccProfileResult profileResult = qt_adapter::createSrgbIccProfile();
    ASSERT_TRUE(profileResult.hasValue());

    QImage sourceImage(QSize(1, 1), QImage::Format_RGBA8888);
    sourceImage.fill(Qt::black);

    const ColorImageResult transformResult =
        qt_adapter::transformIccImage(sourceImage, QByteArrayLiteral("invalid ICC profile"), profileResult.value());

    ASSERT_TRUE(transformResult.hasError());
    EXPECT_EQ(types::ErrorCode::DecodeFailed, transformResult.error().code);
}

TEST(Color, QtAdapterMatchesNeutralLogicalPixels)
{
    const IccProfileResult sourceProfile = qt_adapter::createRgbIccProfile(RgbColorSpace::Srgb);
    const IccProfileResult destinationProfile = qt_adapter::createRgbIccProfile(RgbColorSpace::DisplayP3);
    ASSERT_TRUE(sourceProfile.hasValue());
    ASSERT_TRUE(destinationProfile.hasValue());

    QImage sourceImage(QSize(3, 2), QImage::Format_ARGB32);
    sourceImage.setPixelColor(0, 0, QColor(16, 128, 240, 77));
    sourceImage.setPixelColor(1, 0, QColor(255, 0, 63, 201));
    sourceImage.setPixelColor(2, 0, QColor(41, 211, 92, 19));
    sourceImage.setPixelColor(0, 1, QColor(91, 32, 170, 255));
    sourceImage.setPixelColor(1, 1, QColor(4, 9, 250, 121));
    sourceImage.setPixelColor(2, 1, QColor(222, 187, 33, 64));

    const ColorImageResult adapterResult =
        qt_adapter::transformIccImage(sourceImage, sourceProfile.value(), destinationProfile.value());
    ASSERT_TRUE(adapterResult.hasValue());

    const QImage sourceRgba = sourceImage.convertToFormat(QImage::Format_RGBA8888);
    constexpr std::size_t DestinationStride = 16;
    std::vector<std::uint8_t> destinationStorage(DestinationStride * 2, 0xA7);
    const std::optional<image::ConstRgba8ImageView> sourceView = image::ConstRgba8ImageView::create(
        3,
        2,
        static_cast<std::size_t>(sourceRgba.bytesPerLine()),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(sourceRgba.constBits()),
                                      static_cast<std::size_t>(sourceRgba.sizeInBytes())));
    const std::optional<image::MutableRgba8ImageView> destinationView =
        image::MutableRgba8ImageView::create(3, 2, DestinationStride, destinationStorage);
    ASSERT_TRUE(sourceView.has_value());
    ASSERT_TRUE(destinationView.has_value());

    const std::span<const std::uint8_t> sourceProfileView(
        reinterpret_cast<const std::uint8_t*>(sourceProfile.value().constData()),
        static_cast<std::size_t>(sourceProfile.value().size()));
    const std::span<const std::uint8_t> destinationProfileView(
        reinterpret_cast<const std::uint8_t*>(destinationProfile.value().constData()),
        static_cast<std::size_t>(destinationProfile.value().size()));
    const ColorProcessingResult neutralResult =
        transformIccRgba8(*sourceView, *destinationView, sourceProfileView, destinationProfileView);
    ASSERT_TRUE(neutralResult.hasValue());

    for (std::uint32_t row = 0; row < 2; ++row)
    {
        const std::span<const std::uint8_t> adapterRow(
            reinterpret_cast<const std::uint8_t*>(adapterResult.value().constScanLine(static_cast<int>(row))), 12);
        EXPECT_TRUE(std::ranges::equal(adapterRow, destinationView->rowBytes(row)));
    }
}

}  // namespace
}  // namespace flexraw::core::color

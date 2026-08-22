#include <QColor>
#include <QColorSpace>

#include <gtest/gtest.h>

#include "color.h"

namespace flexraw::core::color
{
namespace
{

TEST(Color, CreatesSrgbProfile)
{
    const IccProfileResult profileResult = createSrgbIccProfile();

    ASSERT_TRUE(profileResult.hasValue());
    EXPECT_FALSE(profileResult.value().isEmpty());
}

TEST(Color, CreatesStandardRgbProfiles)
{
    for (const RgbColorSpace colorSpace : {RgbColorSpace::Srgb, RgbColorSpace::AdobeRgb, RgbColorSpace::DisplayP3})
    {
        const IccProfileResult profileResult = createRgbIccProfile(colorSpace);

        ASSERT_TRUE(profileResult.hasValue());
        EXPECT_TRUE(QColorSpace::fromIccProfile(profileResult.value()).isValid());
    }
}

TEST(Color, PreservesRgbaPixelsForSrgbIdentityTransform)
{
    const IccProfileResult profileResult = createSrgbIccProfile();
    ASSERT_TRUE(profileResult.hasValue());

    QImage sourceImage(QSize(2, 1), QImage::Format_RGBA8888);
    sourceImage.setPixelColor(0, 0, QColor(16, 128, 240, 77));
    sourceImage.setPixelColor(1, 0, QColor(255, 0, 63, 201));

    const ColorImageResult transformResult =
        transformIccImage(sourceImage, profileResult.value(), profileResult.value());

    ASSERT_TRUE(transformResult.hasValue());
    EXPECT_EQ(sourceImage.pixelColor(0, 0), transformResult.value().pixelColor(0, 0));
    EXPECT_EQ(sourceImage.pixelColor(1, 0), transformResult.value().pixelColor(1, 0));
}

TEST(Color, RejectsEmptySourceImage)
{
    const IccProfileResult profileResult = createSrgbIccProfile();
    ASSERT_TRUE(profileResult.hasValue());

    const ColorImageResult transformResult = transformIccImage(QImage(), profileResult.value(), profileResult.value());

    ASSERT_TRUE(transformResult.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, transformResult.error().code);
}

TEST(Color, RejectsInvalidIccProfile)
{
    const IccProfileResult profileResult = createSrgbIccProfile();
    ASSERT_TRUE(profileResult.hasValue());

    QImage sourceImage(QSize(1, 1), QImage::Format_RGBA8888);
    sourceImage.fill(Qt::black);

    const ColorImageResult transformResult =
        transformIccImage(sourceImage, QByteArrayLiteral("invalid ICC profile"), profileResult.value());

    ASSERT_TRUE(transformResult.hasError());
    EXPECT_EQ(types::ErrorCode::DecodeFailed, transformResult.error().code);
}

}  // namespace
}  // namespace flexraw::core::color

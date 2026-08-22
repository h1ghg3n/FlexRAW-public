#include <QColor>
#include <QImage>

#include <gtest/gtest.h>

#include "histogram.h"

namespace flexraw::core::develop
{
namespace
{

TEST(Histogram, CountsRgbAndLuminanceBins)
{
    QImage image(QSize(2, 1), QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, QColor(255, 0, 0, 42));
    image.setPixelColor(1, 0, QColor(0, 255, 0, 200));

    const ImageHistogramResult result = calculateImageHistogram(image);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(2U, result.value().pixelCount);
    EXPECT_EQ(1U, result.value().red[255]);
    EXPECT_EQ(1U, result.value().red[0]);
    EXPECT_EQ(1U, result.value().green[255]);
    EXPECT_EQ(1U, result.value().green[0]);
    EXPECT_EQ(2U, result.value().blue[0]);
    EXPECT_EQ(1U, result.value().luminance[54]);
    EXPECT_EQ(1U, result.value().luminance[182]);
}

TEST(Histogram, RejectsNullImage)
{
    const ImageHistogramResult result = calculateImageHistogram(QImage{});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

}  // namespace
}  // namespace flexraw::core::develop

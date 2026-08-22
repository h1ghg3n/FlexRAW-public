#include <QColor>
#include <QImage>

#include <gtest/gtest.h>

#include "clipping.h"

namespace flexraw::core::develop
{
namespace
{

TEST(Clipping, CountsShadowAndAnyChannelHighlightPixels)
{
    QImage image(QSize(4, 1), QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, QColor(2, 1, 0, 255));
    image.setPixelColor(1, 0, QColor(253, 30, 20, 255));
    image.setPixelColor(2, 0, QColor(10, 10, 10, 255));
    image.setPixelColor(3, 0, QColor(250, 252, 252, 255));

    const ClippingSummaryResult result = calculateClippingSummary(image);

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(4U, result.value().pixelCount);
    EXPECT_EQ(1U, result.value().shadowPixelCount);
    EXPECT_EQ(1U, result.value().highlightPixelCount);
}

TEST(Clipping, RejectsNullImage)
{
    const ClippingSummaryResult result = calculateClippingSummary(QImage{});

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

}  // namespace
}  // namespace flexraw::core::develop

#include <QColor>
#include <QImage>

#include <gtest/gtest.h>

#include "display_frame_qt_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

TEST(DisplayFrameQtAdapterTest, PreservesTopDownColorAndForcesOpaqueBgraPixels)
{
    QImage source(2, 2, QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(10, 20, 30, 40));
    source.setPixelColor(1, 0, QColor(50, 60, 70, 80));
    source.setPixelColor(0, 1, QColor(90, 100, 110, 120));
    source.setPixelColor(1, 1, QColor(130, 140, 150, 160));

    const std::optional<core::client::DisplayFrame> frame = toDisplayFrame(source, 9);

    ASSERT_TRUE(frame.has_value());
    ASSERT_EQ(8U, frame->rowStride());
    const std::span<const std::uint8_t> firstRow = frame->rowBytes(0);
    const std::span<const std::uint8_t> secondRow = frame->rowBytes(1);
    EXPECT_EQ(30U, firstRow[0]);
    EXPECT_EQ(20U, firstRow[1]);
    EXPECT_EQ(10U, firstRow[2]);
    EXPECT_EQ(255U, firstRow[3]);
    EXPECT_EQ(110U, secondRow[0]);
    EXPECT_EQ(100U, secondRow[1]);
    EXPECT_EQ(90U, secondRow[2]);
    EXPECT_EQ(255U, secondRow[3]);
    EXPECT_EQ(9U, frame->previewRevision());

    const QImage restored = toQImage(*frame);
    ASSERT_FALSE(restored.isNull());
    EXPECT_EQ(QColor(10, 20, 30, 255), restored.pixelColor(0, 0));
    EXPECT_EQ(QColor(50, 60, 70, 255), restored.pixelColor(1, 0));
    EXPECT_EQ(QColor(90, 100, 110, 255), restored.pixelColor(0, 1));
    EXPECT_EQ(QColor(130, 140, 150, 255), restored.pixelColor(1, 1));
}

TEST(DisplayFrameQtAdapterTest, RejectsNullQtImage)
{
    EXPECT_FALSE(toDisplayFrame({}, 1).has_value());
}

}  // namespace
}  // namespace flexraw::ui::facade

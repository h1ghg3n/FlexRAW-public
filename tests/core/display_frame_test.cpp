#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "display_frame.h"

namespace flexraw::core::client
{
namespace
{

TEST(DisplayFrameTest, OwnsValidatedImmutableSharedBufferWithExplicitStride)
{
    std::vector<std::uint8_t> bytes(24, 0);
    bytes[0] = 10;
    bytes[3] = 255;
    bytes[7] = 255;
    bytes[12] = 20;
    bytes[15] = 255;
    bytes[19] = 255;

    std::optional<DisplayFrame> created = DisplayFrame::create(2, 2, 12, 7, std::move(bytes));

    ASSERT_TRUE(created.has_value());
    const DisplayFrame copied = *created;
    EXPECT_EQ(2U, copied.width());
    EXPECT_EQ(2U, copied.height());
    EXPECT_EQ(12U, copied.rowStride());
    EXPECT_EQ(7U, copied.previewRevision());
    EXPECT_EQ(DisplayPixelFormat::Bgra8, copied.pixelFormat());
    EXPECT_EQ(ColorEncoding::Srgb, copied.colorEncoding());
    ASSERT_EQ(24U, copied.bytes().size());
    EXPECT_EQ(10U, copied.rowBytes(0)[0]);
    EXPECT_EQ(20U, copied.rowBytes(1)[0]);
    EXPECT_EQ(created->bytes().data(), copied.bytes().data());
    EXPECT_TRUE(copied.rowBytes(2).empty());
}

TEST(DisplayFrameTest, RejectsInvalidDimensionsStrideAndByteSize)
{
    EXPECT_FALSE(DisplayFrame::create(0, 1, 4, 1, std::vector<std::uint8_t>(4)).has_value());
    EXPECT_FALSE(DisplayFrame::create(1, 0, 4, 1, {}).has_value());
    EXPECT_FALSE(DisplayFrame::create(2, 1, 7, 1, std::vector<std::uint8_t>(7)).has_value());
    EXPECT_FALSE(DisplayFrame::create(2, 1, 8, 1, std::vector<std::uint8_t>(7)).has_value());
    EXPECT_FALSE(DisplayFrame::create(1, 1, 4, 1, std::vector<std::uint8_t>(4, 0)).has_value());
    EXPECT_FALSE(DisplayFrame::create(1, 2, std::numeric_limits<std::size_t>::max(), 1, {}).has_value());
}

}  // namespace
}  // namespace flexraw::core::client

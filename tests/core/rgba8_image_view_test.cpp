#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "rgba8_image_view.h"

namespace flexraw::core::image
{
namespace
{

TEST(Rgba8ImageViewTest, CreatesBoundedReadOnlyViewFromPaddedStorage)
{
    constexpr std::uint32_t Width = 2;
    constexpr std::uint32_t Height = 2;
    constexpr std::size_t RowStride = 12;
    std::vector<std::uint8_t> storage(RowStride * Height + 8, 17);

    const std::optional<ConstRgba8ImageView> view = ConstRgba8ImageView::create(Width, Height, RowStride, storage);

    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->width(), Width);
    EXPECT_EQ(view->height(), Height);
    EXPECT_EQ(view->rowStride(), RowStride);
    EXPECT_EQ(view->bytes().size(), RowStride * Height);
    EXPECT_EQ(view->bytes().data(), storage.data());
    EXPECT_EQ(view->rowBytes(1).data(), storage.data() + RowStride);
    EXPECT_EQ(view->rowBytes(1).size(), Width * 4);
    EXPECT_TRUE(view->rowBytes(Height).empty());
}

TEST(Rgba8ImageViewTest, MutableViewWritesThroughCallerOwnedStorage)
{
    constexpr std::size_t RowStride = 8;
    std::vector<std::uint8_t> storage(RowStride * 2, 0);

    const std::optional<MutableRgba8ImageView> view = MutableRgba8ImageView::create(2, 2, RowStride, storage);

    ASSERT_TRUE(view.has_value());
    view->rowBytes(1)[3] = 91;
    EXPECT_EQ(storage[RowStride + 3], 91);
    EXPECT_EQ(view->bytes().size(), storage.size());
}

TEST(Rgba8ImageViewTest, RejectsEmptyGeometryAndStorage)
{
    std::vector<std::uint8_t> storage(16, 0);

    EXPECT_FALSE(ConstRgba8ImageView::create(0, 1, 4, storage).has_value());
    EXPECT_FALSE(ConstRgba8ImageView::create(1, 0, 4, storage).has_value());
    EXPECT_FALSE(ConstRgba8ImageView::create(1, 1, 4, std::span<const std::uint8_t>{}).has_value());
    EXPECT_FALSE(MutableRgba8ImageView::create(1, 1, 4, std::span<std::uint8_t>{}).has_value());
}

TEST(Rgba8ImageViewTest, RejectsShortStrideAndBackingStorage)
{
    std::vector<std::uint8_t> storage(16, 0);

    EXPECT_FALSE(ConstRgba8ImageView::create(2, 2, 7, storage).has_value());
    EXPECT_FALSE(ConstRgba8ImageView::create(2, 3, 8, storage).has_value());
    EXPECT_FALSE(MutableRgba8ImageView::create(2, 2, 7, storage).has_value());
    EXPECT_FALSE(MutableRgba8ImageView::create(2, 3, 8, storage).has_value());
}

TEST(Rgba8ImageViewTest, RejectsOverflowingStorageLayout)
{
    std::vector<std::uint8_t> storage(4, 0);
    const std::size_t overflowingStride = std::numeric_limits<std::size_t>::max();

    EXPECT_FALSE(ConstRgba8ImageView::create(1, 2, overflowingStride, storage).has_value());
    EXPECT_FALSE(MutableRgba8ImageView::create(1, 2, overflowingStride, storage).has_value());
}

}  // namespace
}  // namespace flexraw::core::image

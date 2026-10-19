#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "owned_rgba8_image.h"

namespace flexraw::core::image
{
namespace
{

static_assert(!std::is_copy_constructible_v<OwnedRgba8Image>);
static_assert(!std::is_copy_assignable_v<OwnedRgba8Image>);
static_assert(std::is_nothrow_move_constructible_v<OwnedRgba8Image>);

TEST(OwnedRgba8ImagePortableTest, AllocatesTightStorageAndProjectsViews)
{
    std::optional<OwnedRgba8Image> image = OwnedRgba8Image::allocate(2, 2);

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(2U, image->width());
    EXPECT_EQ(2U, image->height());
    EXPECT_EQ(8U, image->rowStride());
    EXPECT_EQ(16U, image->bytes().size());

    MutableRgba8ImageView writable = image->mutableView();
    writable.rowBytes(1)[3] = 77U;
    EXPECT_EQ(77U, image->constView().rowBytes(1)[3]);
}

TEST(OwnedRgba8ImagePortableTest, PreservesValidatedRowPadding)
{
    std::vector<std::uint8_t> storage(24U, 0U);
    storage[16] = 91U;
    std::optional<OwnedRgba8Image> image = OwnedRgba8Image::create(2, 2, 12, std::move(storage));

    ASSERT_TRUE(image.has_value());
    EXPECT_EQ(12U, image->rowStride());
    EXPECT_EQ(91U, image->constView().rowBytes(1)[4]);
}

TEST(OwnedRgba8ImagePortableTest, RejectsInvalidGeometryStrideAndStorage)
{
    EXPECT_FALSE(OwnedRgba8Image::allocate(0, 1).has_value());
    EXPECT_FALSE(
        OwnedRgba8Image::allocate(std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max())
            .has_value());
    EXPECT_FALSE(OwnedRgba8Image::create(2, 1, 7, std::vector<std::uint8_t>(8)).has_value());
    EXPECT_FALSE(OwnedRgba8Image::create(2, 1, 8, std::vector<std::uint8_t>(7)).has_value());
    EXPECT_FALSE(OwnedRgba8Image::create(2, 1, 8, std::vector<std::uint8_t>(9)).has_value());
}

}  // namespace
}  // namespace flexraw::core::image

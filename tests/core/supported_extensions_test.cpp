#include "supported_extensions.h"

#include <gtest/gtest.h>

namespace flexraw::core::util {
namespace {

TEST(SupportedExtensions, ClassifiesRawExtensions)
{
    EXPECT_EQ(types::SupportedFileKind::Raw, classifyExtension("cr2"));
    EXPECT_EQ(types::SupportedFileKind::Raw, classifyExtension(".CR3"));
    EXPECT_EQ(types::SupportedFileKind::Raw, classifyExtension("NEF"));
    EXPECT_EQ(types::SupportedFileKind::Raw, classifyExtension(" arq "));
    EXPECT_EQ(types::SupportedFileKind::Raw, classifyExtension("nefx"));
    EXPECT_EQ(types::SupportedFileKind::Raw, classifyExtension("ori"));
}

TEST(SupportedExtensions, ClassifiesRasterImageExtensions)
{
    EXPECT_EQ(types::SupportedFileKind::RasterImage, classifyExtension("jpg"));
    EXPECT_EQ(types::SupportedFileKind::RasterImage, classifyExtension(".JPEG"));
    EXPECT_EQ(types::SupportedFileKind::RasterImage, classifyExtension(" tif "));
}

TEST(SupportedExtensions, RejectsUnsupportedExtensions)
{
    EXPECT_EQ(types::SupportedFileKind::Unknown, classifyExtension(""));
    EXPECT_EQ(types::SupportedFileKind::Unknown, classifyExtension("."));
    EXPECT_EQ(types::SupportedFileKind::Unknown, classifyExtension("txt"));
    EXPECT_FALSE(isSupportedExtension("tar.gz"));
}

TEST(SupportedExtensions, ReportsSupportedExtensions)
{
    EXPECT_TRUE(isSupportedExtension(".dng"));
    EXPECT_TRUE(isSupportedExtension("PNG"));
    EXPECT_FALSE(isSupportedExtension("xmp"));
}

} // namespace
} // namespace flexraw::core::util

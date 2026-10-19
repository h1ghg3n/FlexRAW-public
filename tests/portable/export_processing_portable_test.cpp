#include <limits>

#include <gtest/gtest.h>

#include "export_processing.h"

namespace flexraw::core::export_
{
namespace
{

TEST(ExportProcessingPortableTest, AcceptsCurrentSupportedRecipes)
{
    RasterExportOptions jpeg;

    RasterExportOptions png;
    png.format = RasterExportFormat::Png;

    RasterExportOptions tiff;
    tiff.format = RasterExportFormat::Tiff;

    EXPECT_FALSE(findRasterExportOptionsValidationIssue(jpeg).has_value());
    EXPECT_FALSE(findRasterExportOptionsValidationIssue(png).has_value());
    EXPECT_FALSE(findRasterExportOptionsValidationIssue(tiff).has_value());
}

TEST(ExportProcessingPortableTest, RejectsNegativeMaximumDimensionBeforeFormatDetails)
{
    RasterExportOptions options;
    options.maximumDimension = -1;
    options.jpegQuality = 0;

    EXPECT_EQ(RasterExportOptionsValidationIssue::MaximumDimensionNegative,
              findRasterExportOptionsValidationIssue(options));
}

TEST(ExportProcessingPortableTest, RejectsFormatSpecificOptionRanges)
{
    RasterExportOptions jpeg;
    jpeg.jpegQuality = 0;

    RasterExportOptions png;
    png.format = RasterExportFormat::Png;
    png.pngCompression = 10;

    RasterExportOptions tiff;
    tiff.format = RasterExportFormat::Tiff;
    tiff.tiffCompression = static_cast<TiffCompression>(std::numeric_limits<int>::max());

    EXPECT_EQ(RasterExportOptionsValidationIssue::JpegQualityOutOfRange, findRasterExportOptionsValidationIssue(jpeg));
    EXPECT_EQ(RasterExportOptionsValidationIssue::PngCompressionOutOfRange,
              findRasterExportOptionsValidationIssue(png));
    EXPECT_EQ(RasterExportOptionsValidationIssue::TiffCompressionInvalid, findRasterExportOptionsValidationIssue(tiff));
}

TEST(ExportProcessingPortableTest, DistinguishesUnsupportedFormatAndColorSpace)
{
    RasterExportOptions format;
    format.format = static_cast<RasterExportFormat>(std::numeric_limits<int>::max());

    RasterExportOptions colorSpace;
    colorSpace.outputColorSpace = static_cast<RasterOutputColorSpace>(std::numeric_limits<int>::max());

    EXPECT_EQ(RasterExportOptionsValidationIssue::FormatUnsupported, findRasterExportOptionsValidationIssue(format));
    EXPECT_EQ(RasterExportOptionsValidationIssue::ColorSpaceUnsupported,
              findRasterExportOptionsValidationIssue(colorSpace));
}

}  // namespace
}  // namespace flexraw::core::export_

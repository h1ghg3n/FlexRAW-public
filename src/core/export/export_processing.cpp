#include "export_processing.h"

namespace flexraw::core::export_
{
namespace
{

constexpr int MinimumJpegQuality = 1;
constexpr int MaximumJpegQuality = 100;
constexpr int MinimumPngCompression = 0;
constexpr int MaximumPngCompression = 9;

}  // namespace

// 목적: raster export recipe에서 첫 번째 허용 범위 위반을 neutral 값으로 식별
// 입력: options: 검사할 format, encoding, resize와 color option
// 출력: 유효하면 nullopt, 유효하지 않으면 첫 번째 validation issue
std::optional<RasterExportOptionsValidationIssue> findRasterExportOptionsValidationIssue(
    const RasterExportOptions& options) noexcept
{
    if (options.maximumDimension < 0)
    {
        return RasterExportOptionsValidationIssue::MaximumDimensionNegative;
    }

    switch (options.format)
    {
    case RasterExportFormat::Jpeg:
        if (options.jpegQuality < MinimumJpegQuality || options.jpegQuality > MaximumJpegQuality)
        {
            return RasterExportOptionsValidationIssue::JpegQualityOutOfRange;
        }
        break;
    case RasterExportFormat::Png:
        if (options.pngCompression < MinimumPngCompression || options.pngCompression > MaximumPngCompression)
        {
            return RasterExportOptionsValidationIssue::PngCompressionOutOfRange;
        }
        break;
    case RasterExportFormat::Tiff:
        if (options.tiffCompression != TiffCompression::None && options.tiffCompression != TiffCompression::Lzw)
        {
            return RasterExportOptionsValidationIssue::TiffCompressionInvalid;
        }
        break;
    default:
        return RasterExportOptionsValidationIssue::FormatUnsupported;
    }

    if (options.outputColorSpace != RasterOutputColorSpace::Srgb &&
        options.outputColorSpace != RasterOutputColorSpace::AdobeRgb &&
        options.outputColorSpace != RasterOutputColorSpace::DisplayP3)
    {
        return RasterExportOptionsValidationIssue::ColorSpaceUnsupported;
    }

    return std::nullopt;
}

}  // namespace flexraw::core::export_

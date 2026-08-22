#pragma once

namespace flexraw::core::export_
{

enum class RasterExportFormat
{
    Jpeg,
    Png,
    Tiff,
};

enum class TiffCompression
{
    None,
    Lzw,
};

enum class RasterOutputColorSpace
{
    Srgb,
    AdobeRgb,
    DisplayP3,
};

struct RasterExportOptions
{
    RasterExportFormat format{RasterExportFormat::Jpeg};
    int jpegQuality{90};
    int pngCompression{6};
    TiffCompression tiffCompression{TiffCompression::Lzw};
    int maximumDimension{0};
    RasterOutputColorSpace outputColorSpace{RasterOutputColorSpace::Srgb};
    bool includeMetadata{true};
};

}  // namespace flexraw::core::export_

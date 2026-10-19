#pragma once

#include <cstdint>
#include <optional>

#include "export_options.h"

namespace flexraw::core::export_
{

enum class RasterExportOptionsValidationIssue : std::uint8_t
{
    MaximumDimensionNegative,
    JpegQualityOutOfRange,
    PngCompressionOutOfRange,
    TiffCompressionInvalid,
    FormatUnsupported,
    ColorSpaceUnsupported,
};

// 목적: raster export recipe에서 첫 번째 허용 범위 위반을 neutral 값으로 식별
// 입력: options: 검사할 format, encoding, resize와 color option
// 출력: 유효하면 nullopt, 유효하지 않으면 첫 번째 validation issue
[[nodiscard]] std::optional<RasterExportOptionsValidationIssue> findRasterExportOptionsValidationIssue(
    const RasterExportOptions& options) noexcept;

}  // namespace flexraw::core::export_

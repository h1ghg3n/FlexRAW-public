#pragma once

#include <QImage>
#include <QSize>

#include "error.h"
#include "export_options.h"
#include "operation_types.h"
#include "result.h"

class QIODevice;

namespace flexraw::core::raw
{
struct RawPreviewImage;
}

namespace flexraw::core::export_::qt_adapter
{

using RasterSourceImageResult = types::Result<QImage, types::CoreError>;
using RasterEncodingResult = types::Result<QSize, types::CoreError>;

// 목적: LibRaw decode storage를 소비해 raster export용 sRGB QImage ownership으로 이관
// 입력: rawImage: 더 이상 사용하지 않을 bitmap과 남은 orientation
// 출력: storage ownership, color tag와 orientation이 적용된 image 또는 bitmap 구조 오류
[[nodiscard]] RasterSourceImageResult consumeRawForRasterExport(raw::RawPreviewImage&& rawImage);

// 목적: QImage export source를 color/resize 처리하고 지정 device에 raster format으로 encode
// 입력: image/device/options: source, 열린 출력 device와 검증할 recipe, cancellationToken: optional 중단 상태
// 출력: encoded pixel 크기 또는 image/color/encoding/cancellation 실패 정보
[[nodiscard]] RasterEncodingResult encodeRasterImage(const QImage& image,
                                                     QIODevice& device,
                                                     const RasterExportOptions& options,
                                                     const types::CancellationToken* cancellationToken = nullptr);

}  // namespace flexraw::core::export_::qt_adapter

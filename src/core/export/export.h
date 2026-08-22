#pragma once

#include <variant>

#include <QImage>
#include <QString>

#include "develop_params.h"
#include "error.h"
#include "export_options.h"
#include "operation_types.h"
#include "result.h"

namespace flexraw::core::raw
{
struct RawPreviewImage;
}

namespace flexraw::core::export_
{

using RasterExportResult = types::Result<std::monostate, types::CoreError>;
using RasterExportPathResult = types::Result<QString, types::CoreError>;
using RasterSourceImageResult = types::Result<QImage, types::CoreError>;

// 목적: raster export format, encoding과 color option의 value contract 검증
// 입력: options: 검증할 raster export option
// 출력: 성공 표식 또는 유효하지 않은 option 정보
[[nodiscard]] RasterExportResult validateRasterExportOptions(const RasterExportOptions& options);

// 목적: raster output 경로와 parent directory의 기본 파일 시스템 조건 검증
// 입력: outputPath: 검증할 출력 파일 경로
// 출력: 정규화된 절대 경로 또는 구조화된 오류
[[nodiscard]] RasterExportPathResult validateRasterExportPath(const QString& outputPath);

// 목적: LibRaw decode bitmap을 raster export용 독립 sRGB QImage로 변환
// 입력: rawImage: LibRaw가 반환한 bitmap과 남은 orientation
// 출력: color tag와 orientation이 적용된 image 또는 bitmap 구조 오류
[[nodiscard]] RasterSourceImageResult prepareRawForRasterExport(const raw::RawPreviewImage& rawImage);

// 목적: 출력 준비가 끝난 image를 지정한 raster format으로 원자적으로 저장
// 입력: image/outputPath/options: 출력값, metadataSourcePath: EXIF 원본, cancellationToken: optional 중단 상태
// 출력: 성공 표식 또는 입력 검증, 인코딩, metadata, 파일 저장 실패 정보
[[nodiscard]] RasterExportResult writeRasterImage(const QImage& image,
                                                  const QString& outputPath,
                                                  const RasterExportOptions& options,
                                                  const QString& metadataSourcePath = {},
                                                  const types::CancellationToken* cancellationToken = nullptr);

// 목적: RAW file을 full decode하고 develop parameter를 적용해 지정 raster format으로 저장
// 입력: rawPath/params/outputPath/options: RAW 현상 출력값, cancellationToken: optional 중단 상태
// 출력: 성공 표식 또는 RAW decode, develop, output 저장 실패 정보
[[nodiscard]] RasterExportResult writeRawImage(const QString& rawPath,
                                               const types::DevelopParams& params,
                                               const QString& outputPath,
                                               const RasterExportOptions& options,
                                               const types::CancellationToken* cancellationToken = nullptr);

}  // namespace flexraw::core::export_

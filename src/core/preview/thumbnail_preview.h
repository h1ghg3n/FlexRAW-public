#pragma once

#include "file_types.h"
#include "raw_file_reader.h"

#include "error.h"
#include "result.h"

#include <QImage>
#include <QSize>
#include <QString>

namespace flexraw::core::preview {

using ThumbnailPreviewResult = types::Result<QImage, types::CoreError>;

// 목적: LibRaw thumbnail 데이터를 UI가 표시할 QImage로 변환하고 필요 시 downscale
// 입력: thumbnail: LibRaw에서 추출한 thumbnail 데이터, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
[[nodiscard]] ThumbnailPreviewResult createThumbnailPreview(
    const raw::RawThumbnail& thumbnail,
    const QSize& targetSize);

// 목적: LibRaw full decode bitmap을 UI가 표시할 QImage로 변환하고 필요 시 downscale
// 입력: image: LibRaw가 현상한 bitmap 이미지, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
[[nodiscard]] ThumbnailPreviewResult createStandardRawPreview(
    const raw::RawPreviewImage& image,
    const QSize& targetSize);

// 목적: RAW 파일의 embedded thumbnail을 추출해 UI용 QImage preview 생성
// 입력: filePath: 열 RAW 파일 경로, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
[[nodiscard]] ThumbnailPreviewResult loadRawThumbnailPreview(const QString& filePath, const QSize& targetSize);

// 목적: RAW 파일을 표준 preview 품질로 full decode해 UI용 QImage 생성
// 입력: filePath: 열 RAW 파일 경로, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
[[nodiscard]] ThumbnailPreviewResult loadStandardRawPreview(const QString& filePath, const QSize& targetSize);

// 목적: catalog 파일 종류에 맞는 lightweight preview를 생성
// 입력: file: preview를 생성할 지원 파일 정보, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
[[nodiscard]] ThumbnailPreviewResult loadFilePreview(const types::FileDescriptor& file, const QSize& targetSize);

} // namespace flexraw::core::preview

#pragma once

#include "error.h"
#include "result.h"

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QString>

namespace flexraw::core::raw {

enum class ThumbnailFormat {
    Jpeg,
    JpegXl,
    Bitmap,
};

enum class RawPreviewOrientation {
    AsDecoded,
    Rotate180,
    Rotate90CounterClockwise,
    Rotate90Clockwise,
};

struct RawMetadata {
    QString sourcePath;
    QString cameraMake;
    QString cameraModel;
    int width{0};
    int height{0};
    float isoSpeed{0.0F};
    float shutterSeconds{0.0F};
    float aperture{0.0F};
    float focalLengthMillimeters{0.0F};
    QDateTime capturedAt;
};

struct RawThumbnail {
    QByteArray data;
    ThumbnailFormat format{ThumbnailFormat::Jpeg};
    int width{0};
    int height{0};
    int channelCount{0};
    int bitsPerChannel{0};
    RawPreviewOrientation orientation{RawPreviewOrientation::AsDecoded};
};

struct RawDecodeDiagnostics {
    QString sourcePath;
    int sourceFlip{0};
    QSize decodedSize;
    QSize processedSize;
    RawPreviewOrientation appliedOrientation{RawPreviewOrientation::AsDecoded};
};

struct RawPreviewImage {
    QByteArray data;
    int width{0};
    int height{0};
    int channelCount{0};
    int bitsPerChannel{0};
    RawPreviewOrientation orientation{RawPreviewOrientation::AsDecoded};
    RawDecodeDiagnostics diagnostics;
};

using RawMetadataResult = types::Result<RawMetadata, types::CoreError>;
using RawThumbnailResult = types::Result<RawThumbnail, types::CoreError>;
using RawPreviewImageResult = types::Result<RawPreviewImage, types::CoreError>;
using RawDecodeDiagnosticsResult = types::Result<RawDecodeDiagnostics, types::CoreError>;

// 목적: LibRaw로 RAW 파일의 촬영 및 센서 메타데이터 읽기
// 입력: filePath: 열 RAW 파일의 절대 또는 상대 경로
// 출력: RawMetadata 값 또는 구조화된 오류
[[nodiscard]] RawMetadataResult readRawMetadata(const QString& filePath);

// 목적: LibRaw로 RAW 파일에 포함된 embedded thumbnail 추출
// 입력: filePath: 열 RAW 파일의 절대 또는 상대 경로
// 출력: RawThumbnail 값 또는 구조화된 오류
[[nodiscard]] RawThumbnailResult extractEmbeddedThumbnail(const QString& filePath);

// 목적: LibRaw full decode로 standard preview용 RGB bitmap 생성
// 입력: filePath: 열 RAW 파일의 절대 또는 상대 경로
// 출력: RawPreviewImage 값 또는 구조화된 오류
[[nodiscard]] RawPreviewImageResult decodeRawPreviewImage(const QString& filePath);

// 목적: RAW full decode 결과와 orientation 판단 정보를 진단용으로 수집
// 입력: filePath: 진단할 RAW file의 절대 또는 상대 경로
// 출력: decode 크기와 orientation 정보 또는 구조화된 오류
[[nodiscard]] RawDecodeDiagnosticsResult inspectRawDecode(const QString& filePath);

// 목적: LibRaw full decode 이후 남아 있는 camera orientation을 QImage에 적용
// 입력: image: LibRaw bitmap에서 생성한 QImage, orientation: 아직 적용되지 않은 회전 방향
// 출력: orientation이 반영된 QImage
[[nodiscard]] QImage applyRawPreviewOrientation(const QImage& image, RawPreviewOrientation orientation);

} // namespace flexraw::core::raw

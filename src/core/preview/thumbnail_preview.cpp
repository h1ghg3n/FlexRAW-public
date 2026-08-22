#include "thumbnail_preview.h"

#include <QImage>
#include <QImageReader>

#include <limits>
#include <utility>

namespace flexraw::core::preview {
namespace {

// 목적: preview 변환 실패를 설명하는 CoreError 값 생성
// 입력: code: 오류 분류, message: 오류 설명
// 출력: 구조화된 CoreError 값
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, QString message)
{
    return types::CoreError{
        code,
        std::move(message),
    };
}

// 목적: preview target 크기가 유효한지 확인
// 입력: targetSize: 요청된 최대 preview 크기
// 출력: 유효 여부
[[nodiscard]] bool isValidTargetSize(const QSize& targetSize)
{
    return targetSize.width() > 0 && targetSize.height() > 0;
}

// 목적: 압축된 JPEG 또는 JPEG XL thumbnail 데이터를 QImage로 decode
// 입력: thumbnail: 압축된 이미지 데이터를 보유한 RAW thumbnail
// 출력: decode된 QImage 값 또는 구조화된 오류
[[nodiscard]] ThumbnailPreviewResult decodeCompressedThumbnail(const raw::RawThumbnail& thumbnail)
{
    if (thumbnail.data.isEmpty()) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Embedded thumbnail data is empty.")));
    }

    const QImage image = QImage::fromData(thumbnail.data);

    if (image.isNull()) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Unable to decode embedded thumbnail data.")));
    }

    return ThumbnailPreviewResult::success(image);
}

// 목적: LibRaw bitmap thumbnail 데이터를 소유하는 QImage로 복사
// 입력: thumbnail: bitmap 형식의 RAW thumbnail 데이터
// 출력: 독립된 QImage 값 또는 구조화된 오류
[[nodiscard]] ThumbnailPreviewResult copyBitmapThumbnail(const raw::RawThumbnail& thumbnail)
{
    if (thumbnail.width <= 0 || thumbnail.height <= 0 || thumbnail.channelCount <= 0) {
        return ThumbnailPreviewResult::failure(
            makeError(
                types::ErrorCode::DecodeFailed,
                QStringLiteral("Embedded bitmap thumbnail dimensions are invalid.")));
    }

    if (thumbnail.bitsPerChannel != 8) {
        return ThumbnailPreviewResult::failure(
            makeError(
                types::ErrorCode::DecodeFailed,
                QStringLiteral("Embedded bitmap thumbnail bit depth is unsupported.")));
    }

    QImage::Format format = QImage::Format_Invalid;

    switch (thumbnail.channelCount) {
    case 1:
        format = QImage::Format_Grayscale8;
        break;
    case 3:
        format = QImage::Format_RGB888;
        break;
    case 4:
        format = QImage::Format_RGBA8888;
        break;
    default:
        return ThumbnailPreviewResult::failure(
            makeError(
                types::ErrorCode::DecodeFailed,
                QStringLiteral("Embedded bitmap thumbnail channel count is unsupported.")));
    }

    const qsizetype width = thumbnail.width;
    const qsizetype height = thumbnail.height;
    const qsizetype channelCount = thumbnail.channelCount;
    const qsizetype maximumSize = std::numeric_limits<qsizetype>::max();

    if (width > maximumSize / channelCount) {
        return ThumbnailPreviewResult::failure(
            makeError(
                types::ErrorCode::DecodeFailed,
                QStringLiteral("Embedded bitmap thumbnail row size is too large.")));
    }

    const qsizetype bytesPerLine = width * channelCount;

    if (bytesPerLine > maximumSize / height || bytesPerLine > std::numeric_limits<int>::max()) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Embedded bitmap thumbnail size is too large.")));
    }

    const qsizetype requiredSize = bytesPerLine * height;

    if (thumbnail.data.size() < requiredSize) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Embedded bitmap thumbnail data is incomplete.")));
    }

    const QImage borrowedImage(
        reinterpret_cast<const uchar*>(thumbnail.data.constData()),
        thumbnail.width,
        thumbnail.height,
        static_cast<int>(bytesPerLine),
        format);
    const QImage image = borrowedImage.copy();

    if (image.isNull()) {
        return ThumbnailPreviewResult::failure(
            makeError(
                types::ErrorCode::DecodeFailed,
                QStringLiteral("Unable to create embedded bitmap thumbnail image.")));
    }

    return ThumbnailPreviewResult::success(image);
}

// 목적: source image를 target 크기를 넘지 않도록 downscale
// 입력: image: 원본 preview 이미지, targetSize: 허용할 최대 preview 크기
// 출력: 원본 또는 downscale된 QImage 값
[[nodiscard]] QImage downscalePreview(const QImage& image, const QSize& targetSize)
{
    if (image.width() <= targetSize.width() && image.height() <= targetSize.height()) {
        return image;
    }

    return image.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

// 목적: LibRaw thumbnail 데이터를 UI가 표시할 QImage로 변환하고 필요 시 downscale
// 입력: thumbnail: LibRaw에서 추출한 thumbnail 데이터, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
ThumbnailPreviewResult createThumbnailPreview(const raw::RawThumbnail& thumbnail, const QSize& targetSize)
{
    if (!isValidTargetSize(targetSize)) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview target size must be positive.")));
    }

    ThumbnailPreviewResult decodedImage = ThumbnailPreviewResult::failure(
        makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Embedded thumbnail format is unsupported.")));

    switch (thumbnail.format) {
    case raw::ThumbnailFormat::Jpeg:
    case raw::ThumbnailFormat::JpegXl:
        decodedImage = decodeCompressedThumbnail(thumbnail);
        break;
    case raw::ThumbnailFormat::Bitmap:
        decodedImage = copyBitmapThumbnail(thumbnail);
        break;
    }

    if (decodedImage.hasError()) {
        return ThumbnailPreviewResult::failure(decodedImage.error());
    }

    const QImage orientedImage = raw::applyRawPreviewOrientation(decodedImage.value(), thumbnail.orientation);
    return ThumbnailPreviewResult::success(downscalePreview(orientedImage, targetSize));
}

// 목적: LibRaw full decode bitmap을 UI가 표시할 QImage로 변환하고 필요 시 downscale
// 입력: image: LibRaw가 현상한 bitmap 이미지, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
ThumbnailPreviewResult createStandardRawPreview(const raw::RawPreviewImage& image, const QSize& targetSize)
{
    const raw::RawThumbnail bitmap{
        QByteArray::fromRawData(image.data.constData(), image.data.size()),
        raw::ThumbnailFormat::Bitmap,
        image.width,
        image.height,
        image.channelCount,
        image.bitsPerChannel,
    };

    const ThumbnailPreviewResult preview = createThumbnailPreview(bitmap, targetSize);
    if (preview.hasError()) {
        return preview;
    }

    return ThumbnailPreviewResult::success(raw::applyRawPreviewOrientation(preview.value(), image.orientation));
}

// 목적: RAW 파일의 embedded thumbnail을 추출해 UI용 QImage preview 생성
// 입력: filePath: 열 RAW 파일 경로, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
ThumbnailPreviewResult loadRawThumbnailPreview(const QString& filePath, const QSize& targetSize)
{
    if (!isValidTargetSize(targetSize)) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview target size must be positive.")));
    }

    const raw::RawThumbnailResult thumbnail = raw::extractEmbeddedThumbnail(filePath);

    if (thumbnail.hasError()) {
        return ThumbnailPreviewResult::failure(thumbnail.error());
    }

    return createThumbnailPreview(thumbnail.value(), targetSize);
}

// 목적: RAW 파일을 표준 preview 품질로 full decode해 UI용 QImage 생성
// 입력: filePath: 열 RAW 파일 경로, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
ThumbnailPreviewResult loadStandardRawPreview(const QString& filePath, const QSize& targetSize)
{
    if (!isValidTargetSize(targetSize)) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview target size must be positive.")));
    }

    const raw::RawPreviewImageResult image = raw::decodeRawPreviewImage(filePath);

    if (image.hasError()) {
        return ThumbnailPreviewResult::failure(image.error());
    }

    return createStandardRawPreview(image.value(), targetSize);
}

// 목적: catalog 파일 종류에 맞는 lightweight preview를 생성
// 입력: file: preview를 생성할 지원 파일 정보, targetSize: 최대 preview 크기
// 출력: QImage 값 또는 구조화된 오류
ThumbnailPreviewResult loadFilePreview(const types::FileDescriptor& file, const QSize& targetSize)
{
    if (!isValidTargetSize(targetSize)) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Preview target size must be positive.")));
    }

    if (file.kind == types::SupportedFileKind::Raw) {
        return loadRawThumbnailPreview(file.path, targetSize);
    }

    if (file.kind != types::SupportedFileKind::RasterImage) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::UnsupportedFormat, QStringLiteral("Preview file format is unsupported.")));
    }

    QImageReader reader(file.path);
    const QImage image = reader.read();

    if (image.isNull()) {
        return ThumbnailPreviewResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Unable to decode raster image preview.")));
    }

    return ThumbnailPreviewResult::success(downscalePreview(image, targetSize));
}

} // namespace flexraw::core::preview

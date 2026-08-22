#include "raw_file_reader.h"

#include <filesystem>
#include <memory>
#include <utility>

#include <QFileInfo>
#include <QTransform>

#include <libraw/libraw.h>

#include "../util/log.h"
#include "supported_extensions.h"

namespace flexraw::core::raw
{
namespace
{

// 목적: LibRaw 상태 코드에 대응하는 CoreError 값 생성
// 입력: status: LibRaw가 반환한 상태 코드, action: 실패한 RAW 작업 설명
// 출력: 구조화된 CoreError 값
[[nodiscard]] types::CoreError makeLibRawError(int status, const QString& action)
{
    types::ErrorCode code = types::ErrorCode::DecodeFailed;

    if (status == LIBRAW_FILE_UNSUPPORTED)
    {
        code = types::ErrorCode::UnsupportedFormat;
    }
    else if (status == LIBRAW_NO_THUMBNAIL || status == LIBRAW_UNSUPPORTED_THUMBNAIL)
    {
        code = types::ErrorCode::ThumbnailUnavailable;
    }

    return types::CoreError{
        code,
        QStringLiteral("%1: %2").arg(action, QString::fromLatin1(LibRaw::strerror(status))),
    };
}

// 목적: RAW 파일을 열기 전에 경로와 후보 확장자 확인
// 입력: filePath: 확인할 RAW 파일 경로
// 출력: 검증된 QFileInfo 또는 구조화된 오류
[[nodiscard]] types::Result<QFileInfo, types::CoreError> validateRawFilePath(const QString& filePath)
{
    const QString normalizedPath = filePath.trimmed();

    if (normalizedPath.isEmpty())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            types::CoreError{types::ErrorCode::InvalidArgument, QStringLiteral("RAW file path is empty.")});
    }

    const QFileInfo fileInfo(normalizedPath);

    if (!fileInfo.exists())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            types::CoreError{types::ErrorCode::NotFound, QStringLiteral("RAW file does not exist.")});
    }

    if (!fileInfo.isFile())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            types::CoreError{types::ErrorCode::InvalidArgument, QStringLiteral("RAW file path is not a file.")});
    }

    if (!fileInfo.isReadable())
    {
        return types::Result<QFileInfo, types::CoreError>::failure(
            types::CoreError{types::ErrorCode::PermissionDenied, QStringLiteral("RAW file is not readable.")});
    }

    if (util::classifyExtension(fileInfo.suffix()) != types::SupportedFileKind::Raw)
    {
        return types::Result<QFileInfo, types::CoreError>::failure(types::CoreError{
            types::ErrorCode::UnsupportedFormat,
            QStringLiteral("File is not a supported RAW candidate."),
        });
    }

    return types::Result<QFileInfo, types::CoreError>::success(fileInfo);
}

using OpenRawFileResult = types::Result<std::unique_ptr<LibRaw>, types::CoreError>;

// 목적: 검증된 QFileInfo의 경로로 heap 소유 LibRaw 인스턴스 열기
// 입력: fileInfo: 열기 검증을 통과한 RAW 파일 정보
// 출력: 열린 LibRaw 인스턴스 소유권 또는 구조화된 오류
[[nodiscard]] OpenRawFileResult openRawFile(const QFileInfo& fileInfo)
{
    auto decoder = std::make_unique<LibRaw>();
    const std::filesystem::path nativePath = fileInfo.filesystemAbsoluteFilePath();
    const int status = decoder->open_file(nativePath.c_str());

    if (status != LIBRAW_SUCCESS)
    {
        return OpenRawFileResult::failure(makeLibRawError(status, QStringLiteral("Unable to open RAW file")));
    }

    return OpenRawFileResult::success(std::move(decoder));
}

// 목적: LibRaw thumbnail 형식을 Flexraw 공개 형식으로 변환
// 입력: format: LibRaw가 반환한 thumbnail 형식
// 출력: 대응되는 ThumbnailFormat 값
[[nodiscard]] ThumbnailFormat toThumbnailFormat(LibRaw_image_formats format)
{
    switch (format)
    {
    case LIBRAW_IMAGE_JPEG:
        return ThumbnailFormat::Jpeg;
    case LIBRAW_IMAGE_JPEGXL:
        return ThumbnailFormat::JpegXl;
    case LIBRAW_IMAGE_BITMAP:
        return ThumbnailFormat::Bitmap;
    default:
        return ThumbnailFormat::Bitmap;
    }
}

// 목적: LibRaw flip 값을 Flexraw preview orientation으로 변환
// 입력: flip: LibRaw의 0/3/5/6 또는 degree orientation 값
// 출력: 대응하는 RawPreviewOrientation 값
[[nodiscard]] RawPreviewOrientation toRawPreviewOrientation(int flip)
{
    switch (flip)
    {
    case 3:
    case 180:
        return RawPreviewOrientation::Rotate180;
    case 5:
    case 270:
        return RawPreviewOrientation::Rotate90CounterClockwise;
    case 6:
    case 90:
        return RawPreviewOrientation::Rotate90Clockwise;
    default:
        return RawPreviewOrientation::AsDecoded;
    }
}

// 목적: embedded thumbnail이 90도 orientation을 pixel 크기에 이미 반영했는지 확인
// 입력: decoder: RAW sensor 크기와 flip 정보, thumbnail: 추출된 preview 크기
// 출력: source와 thumbnail의 가로·세로 방향이 반대이면 true
[[nodiscard]] bool thumbnailAlreadyQuarterTurned(const LibRaw& decoder, const libraw_processed_image_t& thumbnail)
{
    const int sourceWidth = decoder.imgdata.sizes.width;
    const int sourceHeight = decoder.imgdata.sizes.height;

    if (sourceWidth <= 0 || sourceHeight <= 0 || thumbnail.width == 0 || thumbnail.height == 0 ||
        sourceWidth == sourceHeight || thumbnail.width == thumbnail.height)
    {
        return false;
    }

    return (sourceWidth > sourceHeight) != (thumbnail.width > thumbnail.height);
}

// 목적: thumbnail별 tflip과 main RAW flip을 사용해 embedded preview에 남은 orientation 결정
// 입력: decoder: metadata를 읽은 LibRaw 객체, thumbnail: 추출된 기본 preview
// 출력: embedded thumbnail에 추가 적용할 orientation
[[nodiscard]] RawPreviewOrientation resolveThumbnailOrientation(const LibRaw& decoder,
                                                                const libraw_processed_image_t& thumbnail)
{
    int selectedFlip = decoder.imgdata.sizes.flip;
    bool hasThumbnailSpecificFlip = false;

    if (decoder.imgdata.thumbs_list.thumbcount > 0)
    {
        const unsigned int thumbnailFlip = decoder.imgdata.thumbs_list.thumblist[0].tflip;

        if (thumbnailFlip == 3U || thumbnailFlip == 5U || thumbnailFlip == 6U)
        {
            selectedFlip = static_cast<int>(thumbnailFlip);
            hasThumbnailSpecificFlip = true;
        }
    }

    const RawPreviewOrientation orientation = toRawPreviewOrientation(selectedFlip);
    const bool quarterTurn = orientation == RawPreviewOrientation::Rotate90CounterClockwise ||
                             orientation == RawPreviewOrientation::Rotate90Clockwise;

    if (!hasThumbnailSpecificFlip && quarterTurn && thumbnailAlreadyQuarterTurned(decoder, thumbnail))
    {
        return RawPreviewOrientation::AsDecoded;
    }

    return orientation;
}

}  // namespace

// 목적: LibRaw로 RAW 파일의 촬영 및 센서 메타데이터 읽기
// 입력: filePath: 열 RAW 파일의 절대 또는 상대 경로
// 출력: RawMetadata 값 또는 구조화된 오류
RawMetadataResult readRawMetadata(const QString& filePath)
{
    const types::Result<QFileInfo, types::CoreError> validatedFile = validateRawFilePath(filePath);

    if (validatedFile.hasError())
    {
        return RawMetadataResult::failure(validatedFile.error());
    }

    OpenRawFileResult openedFile = openRawFile(validatedFile.value());

    if (openedFile.hasError())
    {
        return RawMetadataResult::failure(openedFile.error());
    }

    const LibRaw& decoder = *openedFile.value();
    const time_t timestamp = decoder.imgdata.other.timestamp;

    return RawMetadataResult::success(RawMetadata{
        validatedFile.value().absoluteFilePath(),
        QString::fromLatin1(decoder.imgdata.idata.make),
        QString::fromLatin1(decoder.imgdata.idata.model),
        decoder.imgdata.sizes.width,
        decoder.imgdata.sizes.height,
        decoder.imgdata.other.iso_speed,
        decoder.imgdata.other.shutter,
        decoder.imgdata.other.aperture,
        decoder.imgdata.other.focal_len,
        timestamp == 0 ? QDateTime{} : QDateTime::fromSecsSinceEpoch(timestamp),
    });
}

// 목적: LibRaw로 RAW 파일에 포함된 embedded thumbnail 추출
// 입력: filePath: 열 RAW 파일의 절대 또는 상대 경로
// 출력: RawThumbnail 값 또는 구조화된 오류
RawThumbnailResult extractEmbeddedThumbnail(const QString& filePath)
{
    const types::Result<QFileInfo, types::CoreError> validatedFile = validateRawFilePath(filePath);

    if (validatedFile.hasError())
    {
        return RawThumbnailResult::failure(validatedFile.error());
    }

    OpenRawFileResult openedFile = openRawFile(validatedFile.value());

    if (openedFile.hasError())
    {
        return RawThumbnailResult::failure(openedFile.error());
    }

    LibRaw& decoder = *openedFile.value();
    int status = decoder.unpack_thumb();

    if (status != LIBRAW_SUCCESS)
    {
        return RawThumbnailResult::failure(makeLibRawError(status, QStringLiteral("Unable to unpack RAW thumbnail")));
    }

    libraw_processed_image_t* thumbnail = decoder.dcraw_make_mem_thumb(&status);

    if (thumbnail == nullptr || status != LIBRAW_SUCCESS)
    {
        return RawThumbnailResult::failure(makeLibRawError(status, QStringLiteral("Unable to create RAW thumbnail")));
    }

    const RawPreviewOrientation orientation = resolveThumbnailOrientation(decoder, *thumbnail);
    RawThumbnail result{
        QByteArray(reinterpret_cast<const char*>(thumbnail->data), static_cast<qsizetype>(thumbnail->data_size)),
        toThumbnailFormat(thumbnail->type),
        thumbnail->width,
        thumbnail->height,
        thumbnail->colors,
        thumbnail->bits,
        orientation,
    };
    LibRaw::dcraw_clear_mem(thumbnail);

    return RawThumbnailResult::success(std::move(result));
}

// 목적: LibRaw full decode로 standard preview용 RGB bitmap 생성
// 입력: filePath: 열 RAW 파일의 절대 또는 상대 경로
// 출력: RawPreviewImage 값 또는 구조화된 오류
RawPreviewImageResult decodeRawPreviewImage(const QString& filePath)
{
    const types::Result<QFileInfo, types::CoreError> validatedFile = validateRawFilePath(filePath);

    if (validatedFile.hasError())
    {
        return RawPreviewImageResult::failure(validatedFile.error());
    }

    OpenRawFileResult openedFile = openRawFile(validatedFile.value());

    if (openedFile.hasError())
    {
        return RawPreviewImageResult::failure(openedFile.error());
    }

    LibRaw& decoder = *openedFile.value();
    // Develop pipeline의 As Shot와 sRGB 전제에 맞도록 LibRaw output contract를 명시한다.
    decoder.imgdata.params.output_bps = 8;
    decoder.imgdata.params.output_color = 1;
    decoder.imgdata.params.use_camera_wb = 1;
    decoder.imgdata.params.gamm[0] = 1.0 / 2.4;
    decoder.imgdata.params.gamm[1] = 12.92;

    int status = decoder.unpack();

    if (status != LIBRAW_SUCCESS)
    {
        return RawPreviewImageResult::failure(makeLibRawError(status, QStringLiteral("Unable to unpack RAW image")));
    }

    const int decodedWidth = decoder.imgdata.sizes.width;
    const int decodedHeight = decoder.imgdata.sizes.height;
    const int sourceFlip = decoder.imgdata.sizes.flip;

    status = decoder.dcraw_process();

    if (status != LIBRAW_SUCCESS)
    {
        return RawPreviewImageResult::failure(makeLibRawError(status, QStringLiteral("Unable to process RAW image")));
    }

    libraw_processed_image_t* processedImage = decoder.dcraw_make_mem_image(&status);

    if (status != LIBRAW_SUCCESS)
    {
        if (processedImage != nullptr)
        {
            LibRaw::dcraw_clear_mem(processedImage);
        }

        return RawPreviewImageResult::failure(
            makeLibRawError(status, QStringLiteral("Unable to create processed RAW image")));
    }

    if (processedImage == nullptr)
    {
        return RawPreviewImageResult::failure(types::CoreError{
            types::ErrorCode::DecodeFailed,
            QStringLiteral("Processed RAW image buffer is unavailable."),
        });
    }

    if (processedImage->type != LIBRAW_IMAGE_BITMAP)
    {
        LibRaw::dcraw_clear_mem(processedImage);
        return RawPreviewImageResult::failure(types::CoreError{
            types::ErrorCode::DecodeFailed,
            QStringLiteral("Processed RAW image format is unsupported."),
        });
    }

    RawPreviewOrientation orientation = RawPreviewOrientation::AsDecoded;
    const bool outputRemainsUnrotated =
        processedImage->width == decodedWidth && processedImage->height == decodedHeight;
    LOG_DEBUG("raw",
              "RAW decode: flip={}, decoded={}x{}, processed={}x{}, remainsUnrotated={}",
              sourceFlip,
              decodedWidth,
              decodedHeight,
              processedImage->width,
              processedImage->height,
              outputRemainsUnrotated);
    if (outputRemainsUnrotated)
    {
        orientation = toRawPreviewOrientation(sourceFlip);
    }

    RawPreviewImage result{
        QByteArray(reinterpret_cast<const char*>(processedImage->data),
                   static_cast<qsizetype>(processedImage->data_size)),
        processedImage->width,
        processedImage->height,
        processedImage->colors,
        processedImage->bits,
        orientation,
        RawDecodeDiagnostics{
            validatedFile.value().absoluteFilePath(),
            sourceFlip,
            QSize(decodedWidth, decodedHeight),
            QSize(processedImage->width, processedImage->height),
            orientation,
        },
    };
    LibRaw::dcraw_clear_mem(processedImage);

    return RawPreviewImageResult::success(std::move(result));
}

// 목적: RAW full decode 결과와 orientation 판단 정보를 진단용으로 수집
// 입력: filePath: 진단할 RAW file의 절대 또는 상대 경로
// 출력: decode 크기와 orientation 정보 또는 구조화된 오류
RawDecodeDiagnosticsResult inspectRawDecode(const QString& filePath)
{
    RawPreviewImageResult decodedImage = decodeRawPreviewImage(filePath);

    if (decodedImage.hasError())
    {
        return RawDecodeDiagnosticsResult::failure(decodedImage.error());
    }

    return RawDecodeDiagnosticsResult::success(decodedImage.value().diagnostics);
}

// 목적: LibRaw full decode 이후 남아 있는 camera orientation을 QImage에 적용
// 입력: image: LibRaw bitmap에서 생성한 QImage, orientation: 아직 적용되지 않은 회전 방향
// 출력: orientation이 반영된 QImage
QImage applyRawPreviewOrientation(const QImage& image, const RawPreviewOrientation orientation)
{
    switch (orientation)
    {
    case RawPreviewOrientation::Rotate180:
        return image.transformed(QTransform().rotate(180));
    case RawPreviewOrientation::Rotate90CounterClockwise:
        return image.transformed(QTransform().rotate(-90));
    case RawPreviewOrientation::Rotate90Clockwise:
        return image.transformed(QTransform().rotate(90));
    case RawPreviewOrientation::AsDecoded:
        return image;
    }

    return image;
}

}  // namespace flexraw::core::raw

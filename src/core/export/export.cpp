#include "export.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>

#include <QByteArray>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QSaveFile>
#include <QTemporaryFile>

#include <exiv2/exiv2.hpp>

#include "color.h"
#include "develop.h"
#include "raw_file_reader.h"

namespace flexraw::core::export_
{
namespace
{

constexpr int MinimumJpegQuality = 1;
constexpr int MaximumJpegQuality = 100;
constexpr int MinimumPngCompression = 0;
constexpr int MaximumPngCompression = 9;
constexpr int TiffNoCompression = 0;
constexpr int TiffLzwCompression = 1;
constexpr qsizetype CopyChunkSize = 64 * 1024;

// 목적: raster export 실패 정보를 구조화된 CoreError로 생성
// 입력: code: 오류 분류, message: 오류 설명
// 출력: 구성된 CoreError 값
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, QString message)
{
    return {
        code,
        std::move(message),
    };
}

// 목적: optional cancellation token이 중단 요청 상태인지 확인
// 입력: cancellationToken: null일 수 있는 shared cancellation state
// 출력: token이 존재하고 취소 요청됐으면 true
[[nodiscard]] bool isCancellationRequested(const types::CancellationToken* cancellationToken) noexcept
{
    return cancellationToken != nullptr && cancellationToken->isCancellationRequested();
}

// 목적: export stage 사이 cancellation을 구조화된 결과로 변환
// 입력: 없음
// 출력: Cancelled error를 포함한 RasterExportResult
[[nodiscard]] RasterExportResult makeCancelledResult()
{
    return RasterExportResult::failure(
        makeError(types::ErrorCode::Cancelled, QStringLiteral("Raster export cancellation requested.")));
}

// 목적: 저장 대상 경로와 parent directory의 기본 파일 시스템 조건 검증
// 입력: outputPath: 검증할 출력 파일 경로
// 출력: 정규화된 절대 경로 또는 구조화된 오류
[[nodiscard]] types::Result<QString, types::CoreError> validateOutputPath(const QString& outputPath)
{
    if (outputPath.isEmpty() || outputPath != outputPath.trimmed())
    {
        return types::Result<QString, types::CoreError>::failure(
            makeError(types::ErrorCode::InvalidArgument,
                      QStringLiteral("Raster export path is empty or contains outer whitespace.")));
    }

    const QFileInfo outputInfo(QDir::cleanPath(outputPath));

    if (outputInfo.exists() && !outputInfo.isFile())
    {
        return types::Result<QString, types::CoreError>::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Raster export path is not a file.")));
    }

    const QFileInfo parentInfo(outputInfo.absolutePath());

    if (!parentInfo.exists())
    {
        return types::Result<QString, types::CoreError>::failure(
            makeError(types::ErrorCode::NotFound, QStringLiteral("Raster export parent directory does not exist.")));
    }

    if (!parentInfo.isDir())
    {
        return types::Result<QString, types::CoreError>::failure(makeError(
            types::ErrorCode::InvalidArgument, QStringLiteral("Raster export parent path is not a directory.")));
    }

    if (!parentInfo.isWritable() || (outputInfo.exists() && !outputInfo.isWritable()))
    {
        return types::Result<QString, types::CoreError>::failure(
            makeError(types::ErrorCode::PermissionDenied, QStringLiteral("Raster export path is not writable.")));
    }

    return types::Result<QString, types::CoreError>::success(outputInfo.absoluteFilePath());
}

// 목적: 존재하지 않는 leaf도 canonical parent 아래의 안정적인 절대 비교 key로 정규화
// 입력: fileInfo: 비교할 file system entry
// 출력: separator와 parent alias를 정규화하고 leaf case는 보존한 absolute path key
[[nodiscard]] QString normalizedExportPathKey(const QFileInfo& fileInfo)
{
    std::error_code error;
    const std::filesystem::path weaklyCanonicalPath =
        std::filesystem::weakly_canonical(fileInfo.filesystemAbsoluteFilePath(), error);
    const QFileInfo comparableInfo = error ? fileInfo : QFileInfo(weaklyCanonicalPath);
    return QDir::cleanPath(QDir::fromNativeSeparators(comparableInfo.absoluteFilePath()));
}

// 목적: source와 output이 같은 file을 가리키는 경우 원본 보존용 Conflict 생성
// 입력: sourcePath/outputPath: export 입력과 최종 출력 경로
// 출력: 충돌하지 않으면 성공, 같은 file이면 Conflict
[[nodiscard]] RasterExportResult validateDistinctSourceAndOutput(const QString& sourcePath, const QString& outputPath)
{
    if (sourcePath.trimmed().isEmpty() || !exportPathsReferToSameFile(sourcePath, outputPath))
    {
        return RasterExportResult::success({});
    }
    return RasterExportResult::failure(makeError(
        types::ErrorCode::Conflict, QStringLiteral("Raster export source and output paths refer to the same file.")));
}

// 목적: 선택한 raster format에 적용되는 인코딩 옵션 범위 검증
// 입력: options: 검증할 format 및 인코딩 설정
// 출력: 성공 표식 또는 유효하지 않은 옵션 정보
[[nodiscard]] RasterExportResult validateOptions(const RasterExportOptions& options)
{
    if (options.maximumDimension < 0)
    {
        return RasterExportResult::failure(makeError(types::ErrorCode::InvalidArgument,
                                                     QStringLiteral("Export maximum dimension must not be negative.")));
    }

    switch (options.format)
    {
    case RasterExportFormat::Jpeg:
        if (options.jpegQuality < MinimumJpegQuality || options.jpegQuality > MaximumJpegQuality)
        {
            return RasterExportResult::failure(makeError(types::ErrorCode::InvalidArgument,
                                                         QStringLiteral("JPEG quality must be between 1 and 100.")));
        }
        break;
    case RasterExportFormat::Png:
        if (options.pngCompression < MinimumPngCompression || options.pngCompression > MaximumPngCompression)
        {
            return RasterExportResult::failure(makeError(types::ErrorCode::InvalidArgument,
                                                         QStringLiteral("PNG compression must be between 0 and 9.")));
        }
        break;
    case RasterExportFormat::Tiff:
        if (options.tiffCompression != TiffCompression::None && options.tiffCompression != TiffCompression::Lzw)
        {
            return RasterExportResult::failure(
                makeError(types::ErrorCode::InvalidArgument, QStringLiteral("TIFF compression option is invalid.")));
        }
        break;
    default:
        return RasterExportResult::failure(
            makeError(types::ErrorCode::UnsupportedFormat, QStringLiteral("Raster export format is unsupported.")));
    }

    if (options.outputColorSpace != RasterOutputColorSpace::Srgb &&
        options.outputColorSpace != RasterOutputColorSpace::AdobeRgb &&
        options.outputColorSpace != RasterOutputColorSpace::DisplayP3)
    {
        return RasterExportResult::failure(makeError(types::ErrorCode::UnsupportedFormat,
                                                     QStringLiteral("Raster export color space is unsupported.")));
    }

    return RasterExportResult::success({});
}

// 목적: export 최대 변 길이에 맞춰 필요할 때만 image를 aspect ratio 유지하며 축소
// 입력: image: export할 원본 image, maximumDimension: 0 또는 최대 pixel 길이
// 출력: resize가 적용됐거나 원본 크기가 유지된 QImage
[[nodiscard]] QImage resizeForExport(const QImage& image, int maximumDimension)
{
    if (maximumDimension == 0 || std::max(image.width(), image.height()) <= maximumDimension)
    {
        return image;
    }

    const QSize targetSize = image.size().scaled(maximumDimension, maximumDimension, Qt::KeepAspectRatio);
    return image.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// 목적: 현재 sRGB 작업 image를 선택한 출력 색 공간으로 변환하고 ICC 프로필을 연결
// 입력: sourceImage: sRGB 작업 image, outputColorSpace: export 대상 색 공간
// 출력: 색 변환 및 ICC 연결이 완료된 image 또는 프로필/변환 실패 정보
[[nodiscard]] color::ColorImageResult convertForExport(const QImage& sourceImage,
                                                       RasterOutputColorSpace outputColorSpace)
{
    const color::IccProfileResult sourceProfile = color::createRgbIccProfile(color::RgbColorSpace::Srgb);
    if (sourceProfile.hasError())
    {
        return color::ColorImageResult::failure(sourceProfile.error());
    }

    const color::RgbColorSpace destinationColorSpace = [&]() {
        switch (outputColorSpace)
        {
        case RasterOutputColorSpace::Srgb:
            return color::RgbColorSpace::Srgb;
        case RasterOutputColorSpace::AdobeRgb:
            return color::RgbColorSpace::AdobeRgb;
        case RasterOutputColorSpace::DisplayP3:
            return color::RgbColorSpace::DisplayP3;
        }

        return color::RgbColorSpace::Srgb;
    }();
    const color::IccProfileResult destinationProfile = color::createRgbIccProfile(destinationColorSpace);
    if (destinationProfile.hasError())
    {
        return color::ColorImageResult::failure(destinationProfile.error());
    }

    color::ColorImageResult convertedImage =
        color::transformIccImage(sourceImage, sourceProfile.value(), destinationProfile.value());
    if (convertedImage.hasError())
    {
        return convertedImage;
    }

    const QColorSpace outputProfile = QColorSpace::fromIccProfile(destinationProfile.value());
    if (!outputProfile.isValid())
    {
        return color::ColorImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Failed to load the raster export ICC profile.")));
    }

    convertedImage.value().setColorSpace(outputProfile);
    return convertedImage;
}

// 목적: LibRaw full decode bitmap을 develop 및 export에 사용할 독립 sRGB QImage로 변환
// 입력: rawImage: LibRaw가 반환한 8-bit RGB 또는 RGBA bitmap
// 출력: 복사된 sRGB QImage 또는 bitmap 형식/크기 오류
[[nodiscard]] types::Result<QImage, types::CoreError> createRawExportImage(const raw::RawPreviewImage& rawImage)
{
    if (rawImage.width <= 0 || rawImage.height <= 0 || rawImage.bitsPerChannel != 8)
    {
        return types::Result<QImage, types::CoreError>::failure(makeError(
            types::ErrorCode::DecodeFailed, QStringLiteral("Decoded RAW image dimensions or bit depth are invalid.")));
    }

    QImage::Format format = QImage::Format_Invalid;
    switch (rawImage.channelCount)
    {
    case 3:
        format = QImage::Format_RGB888;
        break;
    case 4:
        format = QImage::Format_RGBA8888;
        break;
    default:
        return types::Result<QImage, types::CoreError>::failure(makeError(
            types::ErrorCode::DecodeFailed, QStringLiteral("Decoded RAW image channel count is unsupported.")));
    }

    const qsizetype width = rawImage.width;
    const qsizetype height = rawImage.height;
    const qsizetype channels = rawImage.channelCount;
    if (width > std::numeric_limits<qsizetype>::max() / channels)
    {
        return types::Result<QImage, types::CoreError>::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Decoded RAW image row size is too large.")));
    }

    const qsizetype bytesPerLine = width * channels;
    if (bytesPerLine > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<qsizetype>::max() / bytesPerLine || rawImage.data.size() < bytesPerLine * height)
    {
        return types::Result<QImage, types::CoreError>::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Decoded RAW image data is incomplete.")));
    }

    const QImage borrowedImage(reinterpret_cast<const uchar*>(rawImage.data.constData()),
                               rawImage.width,
                               rawImage.height,
                               static_cast<int>(bytesPerLine),
                               format);
    QImage image = borrowedImage.copy();
    if (image.isNull())
    {
        return types::Result<QImage, types::CoreError>::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Unable to allocate the decoded RAW image.")));
    }

    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image = raw::applyRawPreviewOrientation(image, rawImage.orientation);
    if (image.isNull())
    {
        return types::Result<QImage, types::CoreError>::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Unable to apply RAW image orientation.")));
    }
    return types::Result<QImage, types::CoreError>::success(std::move(image));
}

// 목적: raster format 값을 QImageWriter format 식별자로 변환
// 입력: format: 변환할 raster export format
// 출력: QImageWriter가 인식하는 format 이름
[[nodiscard]] QByteArray writerFormat(RasterExportFormat format)
{
    switch (format)
    {
    case RasterExportFormat::Jpeg:
        return QByteArrayLiteral("JPEG");
    case RasterExportFormat::Png:
        return QByteArrayLiteral("PNG");
    case RasterExportFormat::Tiff:
        return QByteArrayLiteral("TIFF");
    }

    return {};
}

// 목적: 검증된 format별 옵션을 QImageWriter에 적용
// 입력: writer: 구성할 image writer, options: 검증된 raster export 옵션
// 출력: writer 설정 변경
void configureWriter(QImageWriter& writer, const RasterExportOptions& options)
{
    switch (options.format)
    {
    case RasterExportFormat::Jpeg:
        writer.setQuality(options.jpegQuality);
        break;
    case RasterExportFormat::Png:
        writer.setCompression(options.pngCompression);
        break;
    case RasterExportFormat::Tiff:
        writer.setCompression(options.tiffCompression == TiffCompression::Lzw ? TiffLzwCompression : TiffNoCompression);
        break;
    }
}

// 목적: QImageWriter 오류를 기존 CoreError 분류로 변환
// 입력: writer: 실패 상태와 상세 message를 보유한 image writer
// 출력: 호출자가 처리할 수 있는 구조화된 오류
[[nodiscard]] types::CoreError makeWriterError(const QImageWriter& writer)
{
    types::ErrorCode code = types::ErrorCode::DecodeFailed;

    switch (writer.error())
    {
    case QImageWriter::DeviceError:
        code = types::ErrorCode::PermissionDenied;
        break;
    case QImageWriter::UnsupportedFormatError:
        code = types::ErrorCode::UnsupportedFormat;
        break;
    case QImageWriter::InvalidImageError:
        code = types::ErrorCode::InvalidArgument;
        break;
    case QImageWriter::UnknownError:
        break;
    }

    return makeError(code, QStringLiteral("Unable to encode raster export image: %1").arg(writer.errorString()));
}

// 목적: raster format에 맞는 임시 파일 확장자를 반환
// 입력: format: 임시 파일에 저장할 raster export format
// 출력: Exiv2가 식별할 수 있는 확장자
[[nodiscard]] QString temporaryFileExtension(const RasterExportFormat format)
{
    switch (format)
    {
    case RasterExportFormat::Jpeg:
        return QStringLiteral("jpg");
    case RasterExportFormat::Png:
        return QStringLiteral("png");
    case RasterExportFormat::Tiff:
        return QStringLiteral("tiff");
    }

    return {};
}

// 목적: 지정 raster format이 Exiv2 EXIF write를 지원하는지 확인
// 입력: format: EXIF metadata를 적용할 raster export format
// 출력: JPEG 또는 TIFF면 true, 나머지는 false
[[nodiscard]] bool supportsExifMetadata(const RasterExportFormat format)
{
    return format == RasterExportFormat::Jpeg || format == RasterExportFormat::Tiff;
}

// 목적: 복사한 EXIF를 최종 raster pixel artifact와 일치하도록 정규화
// 입력: exifData: 원본에서 복사한 EXIF 묶음, outputSize: 실제 encoded pixel 크기
// 출력: orientation/dimension 갱신과 source thumbnail 제거가 반영된 EXIF 묶음
void normalizeExifForRasterOutput(Exiv2::ExifData& exifData, const QSize& outputSize)
{
    Exiv2::ExifThumb(exifData).erase();
    exifData["Exif.Image.Orientation"] = static_cast<std::uint16_t>(1);
    exifData["Exif.Image.ImageWidth"] = static_cast<std::uint32_t>(outputSize.width());
    exifData["Exif.Image.ImageLength"] = static_cast<std::uint32_t>(outputSize.height());
    exifData["Exif.Photo.PixelXDimension"] = static_cast<std::uint32_t>(outputSize.width());
    exifData["Exif.Photo.PixelYDimension"] = static_cast<std::uint32_t>(outputSize.height());
}

// 목적: 임시 raster 파일에 원본 EXIF metadata를 복사
// 입력: sourcePath: EXIF 원본, outputPath: 임시 출력, outputSize: 최종 pixel 크기, includeMetadata: 복사 여부
// 출력: pixel 방향에 맞춰 Orientation을 정규화한 성공 표식 또는 Exiv2 metadata 읽기/쓰기 실패 정보
[[nodiscard]] RasterExportResult copyExifMetadata(const QString& sourcePath,
                                                  const QString& outputPath,
                                                  const QSize& outputSize,
                                                  const bool includeMetadata)
{
    if (!includeMetadata || sourcePath.isEmpty())
    {
        return RasterExportResult::success({});
    }

    Exiv2::ExifData sourceExifData;
    try
    {
        const auto sourceImage = Exiv2::ImageFactory::open(sourcePath.toStdString());
        sourceImage->readMetadata();
        sourceExifData = sourceImage->exifData();
    }
    catch (const Exiv2::Error&)
    {
        return RasterExportResult::success({});
    }

    if (sourceExifData.empty())
    {
        return RasterExportResult::success({});
    }

    normalizeExifForRasterOutput(sourceExifData, outputSize);

    try
    {
        const auto outputImage = Exiv2::ImageFactory::open(outputPath.toStdString());
        outputImage->readMetadata();
        outputImage->setExifData(sourceExifData);
        outputImage->writeMetadata();
    }
    catch (const Exiv2::Error& error)
    {
        return RasterExportResult::failure(
            makeError(types::ErrorCode::DecodeFailed,
                      QStringLiteral("Unable to write raster export EXIF metadata: %1").arg(error.what())));
    }

    return RasterExportResult::success({});
}

// 목적: 완성된 임시 raster 파일을 QSaveFile로 복사하여 원자적으로 교체
// 입력: sourcePath: 임시 파일, outputPath: 최종 경로, cancellationToken: optional 중단 상태
// 출력: 성공 표식 또는 임시 파일 읽기/최종 commit 실패 정보
[[nodiscard]] RasterExportResult commitTemporaryOutput(const QString& sourcePath,
                                                       const QString& outputPath,
                                                       const types::CancellationToken* cancellationToken)
{
    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::ReadOnly))
    {
        return RasterExportResult::failure(
            makeError(types::ErrorCode::PermissionDenied,
                      QStringLiteral("Unable to read temporary raster export file: %1").arg(sourceFile.errorString())));
    }

    QSaveFile outputFile(outputPath);
    outputFile.setDirectWriteFallback(false);
    if (!outputFile.open(QIODevice::WriteOnly))
    {
        return RasterExportResult::failure(
            makeError(types::ErrorCode::PermissionDenied,
                      QStringLiteral("Unable to open raster export file: %1").arg(outputFile.errorString())));
    }

    while (!sourceFile.atEnd())
    {
        if (isCancellationRequested(cancellationToken))
        {
            outputFile.cancelWriting();
            return makeCancelledResult();
        }

        const QByteArray chunk = sourceFile.read(CopyChunkSize);
        if (chunk.isEmpty() && sourceFile.error() != QFile::NoError)
        {
            outputFile.cancelWriting();
            return RasterExportResult::failure(makeError(
                types::ErrorCode::DecodeFailed,
                QStringLiteral("Unable to read temporary raster export file: %1").arg(sourceFile.errorString())));
        }

        if (outputFile.write(chunk) != chunk.size())
        {
            outputFile.cancelWriting();
            return RasterExportResult::failure(
                makeError(types::ErrorCode::PermissionDenied,
                          QStringLiteral("Unable to write raster export file: %1").arg(outputFile.errorString())));
        }
    }

    if (isCancellationRequested(cancellationToken))
    {
        outputFile.cancelWriting();
        return makeCancelledResult();
    }

    if (!outputFile.commit())
    {
        return RasterExportResult::failure(
            makeError(types::ErrorCode::PermissionDenied,
                      QStringLiteral("Unable to commit raster export file: %1").arg(outputFile.errorString())));
    }

    return RasterExportResult::success({});
}

}  // namespace

// 목적: raster export format, encoding과 color option의 value contract 검증
// 입력: options: 검증할 raster export option
// 출력: 성공 표식 또는 유효하지 않은 option 정보
RasterExportResult validateRasterExportOptions(const RasterExportOptions& options)
{
    return validateOptions(options);
}

// 목적: raster output 경로와 parent directory의 기본 파일 시스템 조건 검증
// 입력: outputPath: 검증할 출력 파일 경로
// 출력: 정규화된 절대 경로 또는 구조화된 오류
RasterExportPathResult validateRasterExportPath(const QString& outputPath)
{
    return validateOutputPath(outputPath);
}

// 목적: 두 export 경로가 정규화 또는 실제 file identity 기준으로 같은 대상을 가리키는지 판정
// 입력: firstPath/secondPath: 비교할 source 또는 output 경로
// 출력: 같은 경로, symbolic link 또는 hard link 대상이면 true
bool exportPathsReferToSameFile(const QString& firstPath, const QString& secondPath)
{
    if (firstPath.isEmpty() || secondPath.isEmpty() || firstPath != firstPath.trimmed() ||
        secondPath != secondPath.trimmed())
    {
        return false;
    }

    const QFileInfo firstInfo(QDir::cleanPath(firstPath));
    const QFileInfo secondInfo(QDir::cleanPath(secondPath));
    if (normalizedExportPathKey(firstInfo) == normalizedExportPathKey(secondInfo))
    {
        return true;
    }
    if (!firstInfo.exists() || !secondInfo.exists())
    {
        return false;
    }

    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(
        firstInfo.filesystemAbsoluteFilePath(), secondInfo.filesystemAbsoluteFilePath(), error);
    return !error && equivalent;
}

// 목적: LibRaw decode bitmap을 raster export용 독립 sRGB QImage로 변환
// 입력: rawImage: LibRaw가 반환한 bitmap과 남은 orientation
// 출력: color tag와 orientation이 적용된 image 또는 bitmap 구조 오류
RasterSourceImageResult prepareRawForRasterExport(const raw::RawPreviewImage& rawImage)
{
    return createRawExportImage(rawImage);
}

// 목적: 출력 준비가 끝난 image를 지정한 raster format으로 원자적으로 저장
// 입력: image/outputPath/options: 출력값, metadataSourcePath: EXIF 원본, cancellationToken: optional 중단 상태
// 출력: 성공 표식 또는 입력 검증, 인코딩, metadata, 파일 저장 실패 정보
RasterExportResult writeRasterImage(const QImage& image,
                                    const QString& outputPath,
                                    const RasterExportOptions& options,
                                    const QString& metadataSourcePath,
                                    const types::CancellationToken* cancellationToken)
{
    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    if (image.isNull())
    {
        return RasterExportResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Raster export image is empty.")));
    }

    const RasterExportResult validatedOptions = validateRasterExportOptions(options);

    if (validatedOptions.hasError())
    {
        return validatedOptions;
    }

    const RasterExportPathResult validatedPath = validateRasterExportPath(outputPath);

    if (validatedPath.hasError())
    {
        return RasterExportResult::failure(validatedPath.error());
    }

    const RasterExportResult distinctPaths = validateDistinctSourceAndOutput(metadataSourcePath, validatedPath.value());
    if (distinctPaths.hasError())
    {
        return distinctPaths;
    }

    const QFileInfo outputInfo(validatedPath.value());
    QTemporaryFile encodedFile(
        QDir(outputInfo.absolutePath())
            .filePath(QStringLiteral(".flexraw-export-XXXXXX.%1").arg(temporaryFileExtension(options.format))));
    encodedFile.setAutoRemove(true);
    if (!encodedFile.open())
    {
        return RasterExportResult::failure(makeError(
            types::ErrorCode::PermissionDenied,
            QStringLiteral("Unable to create temporary raster export file: %1").arg(encodedFile.errorString())));
    }

    QImageWriter writer(&encodedFile, writerFormat(options.format));
    configureWriter(writer, options);

    const color::ColorImageResult convertedImage = convertForExport(image, options.outputColorSpace);
    if (convertedImage.hasError())
    {
        return RasterExportResult::failure(convertedImage.error());
    }

    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    const QImage outputImage = resizeForExport(convertedImage.value(), options.maximumDimension);

    if (!writer.write(outputImage))
    {
        const types::CoreError error = makeWriterError(writer);
        return RasterExportResult::failure(error);
    }

    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    encodedFile.close();
    const RasterExportResult metadataResult =
        supportsExifMetadata(options.format)
            ? copyExifMetadata(metadataSourcePath, encodedFile.fileName(), outputImage.size(), options.includeMetadata)
            : RasterExportResult::success({});
    if (metadataResult.hasError())
    {
        return metadataResult;
    }

    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    return commitTemporaryOutput(encodedFile.fileName(), validatedPath.value(), cancellationToken);
}

// 목적: RAW file을 full decode하고 develop parameter를 적용해 지정 raster format으로 저장
// 입력: rawPath/params/outputPath/options: RAW 현상 출력값, cancellationToken: optional 중단 상태
// 출력: 성공 표식 또는 RAW decode, develop, output 저장 실패 정보
RasterExportResult writeRawImage(const QString& rawPath,
                                 const types::DevelopParams& params,
                                 const QString& outputPath,
                                 const RasterExportOptions& options,
                                 const types::CancellationToken* cancellationToken)
{
    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    const RasterExportResult validatedOptions = validateRasterExportOptions(options);
    if (validatedOptions.hasError())
    {
        return validatedOptions;
    }

    const RasterExportResult distinctPaths = validateDistinctSourceAndOutput(rawPath, outputPath);
    if (distinctPaths.hasError())
    {
        return distinctPaths;
    }

    const raw::RawPreviewImageResult decodedRaw = raw::decodeRawPreviewImage(rawPath);
    if (decodedRaw.hasError())
    {
        return RasterExportResult::failure(decodedRaw.error());
    }

    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    const RasterSourceImageResult sourceImage = prepareRawForRasterExport(decodedRaw.value());
    if (sourceImage.hasError())
    {
        return RasterExportResult::failure(sourceImage.error());
    }

    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    const develop::DevelopImageResult developedImage = develop::applyDevelop(sourceImage.value(), params);
    if (developedImage.hasError())
    {
        return RasterExportResult::failure(developedImage.error());
    }

    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    return writeRasterImage(developedImage.value(), outputPath, options, rawPath, cancellationToken);
}

}  // namespace flexraw::core::export_

#include "export_qt_adapter.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColorSpace>
#include <QIODevice>
#include <QImageWriter>

#include "color_qt_adapter.h"
#include "export_processing.h"
#include "raw_file_reader.h"

namespace flexraw::core::export_::qt_adapter
{
namespace
{

constexpr int TiffNoCompression = 0;
constexpr int TiffLzwCompression = 1;

// 목적: raster export Qt adapter 실패 정보를 구조화된 CoreError로 생성
// 입력: code: 오류 분류, message: 오류 설명
// 출력: 구성된 CoreError 값
[[nodiscard]] types::CoreError makeError(types::ErrorCode code, QString message)
{
    return {code, std::move(message)};
}

// 목적: neutral raster export validation issue를 기존 Qt adapter CoreError 의미로 투영
// 입력: issue: option 허용 범위 또는 지원 범위 위반
// 출력: 기존 caller가 소비하는 error code와 diagnostic message
[[nodiscard]] types::CoreError projectOptionsValidationIssue(RasterExportOptionsValidationIssue issue)
{
    switch (issue)
    {
    case RasterExportOptionsValidationIssue::MaximumDimensionNegative:
        return makeError(types::ErrorCode::InvalidArgument,
                         QStringLiteral("Export maximum dimension must not be negative."));
    case RasterExportOptionsValidationIssue::JpegQualityOutOfRange:
        return makeError(types::ErrorCode::InvalidArgument, QStringLiteral("JPEG quality must be between 1 and 100."));
    case RasterExportOptionsValidationIssue::PngCompressionOutOfRange:
        return makeError(types::ErrorCode::InvalidArgument, QStringLiteral("PNG compression must be between 0 and 9."));
    case RasterExportOptionsValidationIssue::TiffCompressionInvalid:
        return makeError(types::ErrorCode::InvalidArgument, QStringLiteral("TIFF compression option is invalid."));
    case RasterExportOptionsValidationIssue::FormatUnsupported:
        return makeError(types::ErrorCode::UnsupportedFormat, QStringLiteral("Raster export format is unsupported."));
    case RasterExportOptionsValidationIssue::ColorSpaceUnsupported:
        return makeError(types::ErrorCode::UnsupportedFormat,
                         QStringLiteral("Raster export color space is unsupported."));
    }

    return makeError(types::ErrorCode::Unknown, QStringLiteral("Raster export options are invalid."));
}

// 목적: optional cancellation token이 중단 요청 상태인지 확인
// 입력: cancellationToken: null일 수 있는 shared cancellation state
// 출력: token이 존재하고 취소 요청됐으면 true
[[nodiscard]] bool isCancellationRequested(const types::CancellationToken* cancellationToken) noexcept
{
    return cancellationToken != nullptr && cancellationToken->isCancellationRequested();
}

// 목적: export adapter stage 사이 cancellation을 구조화된 결과로 변환
// 입력: 없음
// 출력: Cancelled error를 포함한 RasterEncodingResult
[[nodiscard]] RasterEncodingResult makeCancelledResult()
{
    return RasterEncodingResult::failure(
        makeError(types::ErrorCode::Cancelled, QStringLiteral("Raster export cancellation requested.")));
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

// 목적: 현재 sRGB 작업 image를 선택한 출력 색 공간으로 변환하고 ICC profile을 연결
// 입력: sourceImage: sRGB 작업 image, outputColorSpace: export 대상 색 공간
// 출력: 색 변환 및 ICC 연결이 완료된 image 또는 profile/변환 실패 정보
[[nodiscard]] color::ColorImageResult convertForExport(const QImage& sourceImage,
                                                       RasterOutputColorSpace outputColorSpace)
{
    const color::IccProfileResult sourceProfile = color::qt_adapter::createRgbIccProfile(color::RgbColorSpace::Srgb);
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
    const color::IccProfileResult destinationProfile = color::qt_adapter::createRgbIccProfile(destinationColorSpace);
    if (destinationProfile.hasError())
    {
        return color::ColorImageResult::failure(destinationProfile.error());
    }

    color::ColorImageResult convertedImage =
        color::qt_adapter::transformIccImage(sourceImage, sourceProfile.value(), destinationProfile.value());
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

// 목적: QImage가 소비한 RAW byte storage를 마지막 shared image까지 소유
// 입력: cleanupInfo: heap에 소유권을 이관한 byte vector
// 출력: 없음
void deleteRawExportStorage(void* cleanupInfo) noexcept
{
    delete static_cast<std::vector<std::uint8_t>*>(cleanupInfo);
}

// 목적: LibRaw full decode bitmap ownership을 develop/export용 sRGB QImage로 이관
// 입력: rawImage: 소비할 8-bit RGB 또는 RGBA bitmap과 pending orientation
// 출력: storage를 공유하는 sRGB QImage 또는 bitmap 형식/크기 오류
[[nodiscard]] RasterSourceImageResult createRawExportImage(raw::RawPreviewImage&& rawImage)
{
    if (rawImage.raster.width() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        rawImage.raster.height() > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return RasterSourceImageResult::failure(makeError(
            types::ErrorCode::DecodeFailed, QStringLiteral("Decoded RAW image dimensions or bit depth are invalid.")));
    }

    QImage::Format format = QImage::Format_Invalid;
    switch (rawImage.raster.layout())
    {
    case raw::RawRasterLayout::Rgb8:
        format = QImage::Format_RGB888;
        break;
    case raw::RawRasterLayout::Rgba8:
        format = QImage::Format_RGBA8888;
        break;
    }

    if (rawImage.raster.rowStride() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return RasterSourceImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Decoded RAW image row size is too large.")));
    }

    const int width = static_cast<int>(rawImage.raster.width());
    const int height = static_cast<int>(rawImage.raster.height());
    const int rowStride = static_cast<int>(rawImage.raster.rowStride());
    const raw::RawPreviewOrientation pendingOrientation = rawImage.raster.pendingOrientation();
    auto storage = std::make_unique<std::vector<std::uint8_t>>(std::move(rawImage.raster).takeBytes());
    QImage image(storage->data(), width, height, rowStride, format, deleteRawExportStorage, storage.get());
    if (image.isNull())
    {
        return RasterSourceImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Unable to allocate the decoded RAW image.")));
    }
    storage.release();

    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image = raw::applyRawPreviewOrientation(image, pendingOrientation);
    if (image.isNull())
    {
        return RasterSourceImageResult::failure(
            makeError(types::ErrorCode::DecodeFailed, QStringLiteral("Unable to apply RAW image orientation.")));
    }
    return RasterSourceImageResult::success(std::move(image));
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

}  // namespace

// 목적: LibRaw decode storage를 소비해 raster export용 sRGB QImage ownership으로 이관
// 입력: rawImage: 더 이상 사용하지 않을 bitmap과 남은 orientation
// 출력: storage ownership, color tag와 orientation이 적용된 image 또는 bitmap 구조 오류
RasterSourceImageResult consumeRawForRasterExport(raw::RawPreviewImage&& rawImage)
{
    return createRawExportImage(std::move(rawImage));
}

// 목적: QImage export source를 color/resize 처리하고 지정 device에 raster format으로 encode
// 입력: image/device/options: source, 열린 출력 device와 검증할 recipe, cancellationToken: optional 중단 상태
// 출력: encoded pixel 크기 또는 image/color/encoding/cancellation 실패 정보
RasterEncodingResult encodeRasterImage(const QImage& image,
                                       QIODevice& device,
                                       const RasterExportOptions& options,
                                       const types::CancellationToken* cancellationToken)
{
    if (image.isNull())
    {
        return RasterEncodingResult::failure(
            makeError(types::ErrorCode::InvalidArgument, QStringLiteral("Raster export image is empty.")));
    }

    if (const std::optional<RasterExportOptionsValidationIssue> issue = findRasterExportOptionsValidationIssue(options))
    {
        return RasterEncodingResult::failure(projectOptionsValidationIssue(*issue));
    }

    QImageWriter writer(&device, writerFormat(options.format));
    configureWriter(writer, options);

    const color::ColorImageResult convertedImage = convertForExport(image, options.outputColorSpace);
    if (convertedImage.hasError())
    {
        return RasterEncodingResult::failure(convertedImage.error());
    }
    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    const QImage outputImage = resizeForExport(convertedImage.value(), options.maximumDimension);
    if (!writer.write(outputImage))
    {
        return RasterEncodingResult::failure(makeWriterError(writer));
    }
    if (isCancellationRequested(cancellationToken))
    {
        return makeCancelledResult();
    }

    return RasterEncodingResult::success(outputImage.size());
}

}  // namespace flexraw::core::export_::qt_adapter

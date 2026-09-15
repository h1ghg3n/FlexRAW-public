#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTemporaryDir>

#include <exiv2/exiv2.hpp>

#include <gtest/gtest.h>

#include "export.h"

namespace flexraw::core::export_
{
namespace
{

// 목적: raster export test에서 format별 저장 결과를 비교할 기준 image 생성
// 입력: 없음
// 출력: 서로 다른 RGBA pixel을 포함한 작은 QImage
[[nodiscard]] QImage makeTestImage()
{
    QImage image(QSize(3, 2), QImage::Format_RGBA8888);
    image.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    image.setPixelColor(1, 0, QColor(0, 255, 0, 128));
    image.setPixelColor(2, 0, QColor(0, 0, 255, 64));
    image.setPixelColor(0, 1, QColor(16, 32, 48, 255));
    image.setPixelColor(1, 1, QColor(96, 112, 128, 192));
    image.setPixelColor(2, 1, QColor(208, 224, 240, 0));
    return image;
}

// 목적: EXIF Artist 항목이 포함된 JPEG metadata 원본을 생성
// 입력: path: EXIF를 기록할 JPEG file 경로
// 출력: EXIF 기록 성공 여부
[[nodiscard]] bool writeExifArtist(const QString& path)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        image->exifData()["Exif.Image.Artist"] = "Flexraw Test";
        image->writeMetadata();
        return true;
    }
    catch (const Exiv2::Error&)
    {
        return false;
    }
}

// 목적: 출력 JPEG EXIF Artist 항목을 읽기
// 입력: path: EXIF를 읽을 JPEG file 경로
// 출력: Artist 문자열 또는 metadata 읽기 실패 시 빈 문자열
[[nodiscard]] QString readExifArtist(const QString& path)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        const Exiv2::ExifData::const_iterator artist = image->exifData().findKey(Exiv2::ExifKey("Exif.Image.Artist"));
        return artist == image->exifData().end() ? QString{} : QString::fromStdString(artist->toString());
    }
    catch (const Exiv2::Error&)
    {
        return {};
    }
}

// 목적: EXIF Orientation 항목을 가진 metadata 원본 생성
// 입력: path: EXIF를 기록할 JPEG file 경로, orientation: 기록할 EXIF orientation 값
// 출력: EXIF 기록 성공 여부
[[nodiscard]] bool writeExifOrientation(const QString& path, const std::uint16_t orientation)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        image->exifData()["Exif.Image.Orientation"] = orientation;
        image->writeMetadata();
        return true;
    }
    catch (const Exiv2::Error&)
    {
        return false;
    }
}

// 목적: artifact 정규화 test용 EXIF dimension과 thumbnail을 구성
// 입력: exifData: 수정할 EXIF 묶음, thumbnailPath: 삽입할 JPEG thumbnail 경로
// 출력: source pixel을 가리키는 의도적인 stale metadata 구성
void configureArtifactExif(Exiv2::ExifData& exifData, const QString& thumbnailPath)
{
    exifData["Exif.Image.Artist"] = "Flexraw Test";
    exifData["Exif.Image.Orientation"] = static_cast<std::uint16_t>(8);
    exifData["Exif.Image.ImageWidth"] = static_cast<std::uint32_t>(640);
    exifData["Exif.Image.ImageLength"] = static_cast<std::uint32_t>(480);
    exifData["Exif.Photo.PixelXDimension"] = static_cast<std::uint32_t>(640);
    exifData["Exif.Photo.PixelYDimension"] = static_cast<std::uint32_t>(480);
    Exiv2::ExifThumb(exifData).setJpegThumbnail(thumbnailPath.toStdString());
}

// 목적: artifact 정규화 test에 사용할 원본 EXIF 묶음 기록
// 입력: path: metadata 원본 image 경로, thumbnailPath: 삽입할 JPEG thumbnail 경로
// 출력: EXIF 기록 성공 여부
[[nodiscard]] bool writeArtifactExif(const QString& path, const QString& thumbnailPath)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        configureArtifactExif(image->exifData(), thumbnailPath);
        image->writeMetadata();
        return true;
    }
    catch (const Exiv2::Error&)
    {
        return false;
    }
}

// 목적: 지정 EXIF unsigned 항목을 출력 artifact에서 읽기
// 입력: path: EXIF를 읽을 file, key: 조회할 EXIF key
// 출력: 항목 값 또는 metadata 읽기 실패·항목 부재 시 0
[[nodiscard]] std::uint32_t readExifUnsigned(const QString& path, const char* key)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        const Exiv2::ExifData::const_iterator value = image->exifData().findKey(Exiv2::ExifKey(key));
        return value == image->exifData().end() ? 0U : value->toUint32();
    }
    catch (const Exiv2::Error&)
    {
        return 0U;
    }
}

// 목적: 출력 artifact에 EXIF thumbnail이 남아 있는지 확인
// 입력: path: EXIF를 읽을 file 경로
// 출력: IFD1 thumbnail data가 있으면 true
[[nodiscard]] bool hasExifThumbnail(const QString& path)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        return Exiv2::ExifThumbC(image->exifData()).copy().size() > 0U;
    }
    catch (const Exiv2::Error&)
    {
        return false;
    }
}

// 목적: JPEG EXIF segment 한도를 넘는 test용 metadata 묶음 구성
// 입력: exifData: 수정할 EXIF 묶음
// 출력: 개별 항목은 허용 범위지만 전체가 JPEG APP1 한도를 넘는 metadata 구성
void configureOversizedExif(Exiv2::ExifData& exifData)
{
    const std::string payload(12'000, 'x');
    exifData["Exif.Image.ImageDescription"] = payload;
    exifData["Exif.Image.Make"] = payload;
    exifData["Exif.Image.Model"] = payload;
    exifData["Exif.Image.Software"] = payload;
    exifData["Exif.Image.Artist"] = payload;
    exifData["Exif.Image.Copyright"] = payload;
}

// 목적: metadata write failure 회귀용 oversized TIFF EXIF 원본 생성
// 입력: path: EXIF를 기록할 TIFF file 경로
// 출력: metadata 원본 생성 성공 여부
[[nodiscard]] bool writeOversizedExif(const QString& path)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        configureOversizedExif(image->exifData());
        image->writeMetadata();
        return true;
    }
    catch (const Exiv2::Error&)
    {
        return false;
    }
}

// 목적: 출력 JPEG의 EXIF Orientation 값 읽기
// 입력: path: EXIF를 읽을 JPEG file 경로
// 출력: Orientation 값 또는 metadata 읽기 실패·항목 부재 시 0
[[nodiscard]] std::uint32_t readExifOrientation(const QString& path)
{
    try
    {
        const auto image = Exiv2::ImageFactory::open(path.toStdString());
        image->readMetadata();
        const Exiv2::ExifData::const_iterator orientation =
            image->exifData().findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
        return orientation == image->exifData().end() ? 0U : orientation->toUint32();
    }
    catch (const Exiv2::Error&)
    {
        return 0U;
    }
}

// 목적: Qt image format plugin을 초기화한 raster export test fixture 제공
// 입력: 없음
// 출력: 없음
class RasterExportTest : public testing::Test
{
protected:
    // 목적: raster export test suite 시작 전에 Qt core application 초기화
    // 입력: 없음
    // 출력: 없음
    static void SetUpTestSuite()
    {
        if (QCoreApplication::instance() != nullptr)
        {
            return;
        }

        static int argumentCount = 1;
        static char applicationName[] = "flexraw_core_tests";
        static char* arguments[] = {applicationName, nullptr};
        static QCoreApplication application(argumentCount, arguments);
    }
};

TEST_F(RasterExportTest, RejectsOuterWhitespaceWithoutChangingTheOutputLocator)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("image.png"));

    const RasterExportPathResult result = validateRasterExportPath(outputPath + QLatin1Char(' '));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(outputPath));
}

TEST_F(RasterExportTest, StoresPngWithoutPixelLoss)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QImage sourceImage = makeTestImage();
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("image.png"));
    RasterExportOptions options;
    options.format = RasterExportFormat::Png;
    options.pngCompression = 9;

    const RasterExportResult result = writeRasterImage(sourceImage, outputPath, options);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    const QImage storedImage(outputPath);
    ASSERT_FALSE(storedImage.isNull());
    const QImage normalizedImage = storedImage.convertToFormat(sourceImage.format());
    ASSERT_EQ(sourceImage.size(), normalizedImage.size());
    ASSERT_EQ(sourceImage.sizeInBytes(), normalizedImage.sizeInBytes());
    EXPECT_EQ(0, std::memcmp(sourceImage.constBits(), normalizedImage.constBits(), sourceImage.sizeInBytes()));
    EXPECT_TRUE(storedImage.colorSpace().isValid());
}

TEST_F(RasterExportTest, WritesDecodableJpeg)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QImage sourceImage = makeTestImage();
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("image.jpg"));
    RasterExportOptions options;
    options.format = RasterExportFormat::Jpeg;
    options.jpegQuality = 85;

    const RasterExportResult result = writeRasterImage(sourceImage, outputPath, options);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    QImageReader reader(outputPath);
    EXPECT_EQ(QByteArrayLiteral("jpeg"), reader.format().toLower());
    const QImage storedImage = reader.read();
    ASSERT_FALSE(storedImage.isNull()) << reader.errorString().toStdString();
    EXPECT_EQ(sourceImage.size(), storedImage.size());
}

TEST_F(RasterExportTest, WritesDecodableTiff)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QImage sourceImage = makeTestImage();
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("image.tiff"));
    RasterExportOptions options;
    options.format = RasterExportFormat::Tiff;
    options.tiffCompression = TiffCompression::Lzw;

    const RasterExportResult result = writeRasterImage(sourceImage, outputPath, options);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    QImageReader reader(outputPath);
    EXPECT_EQ(QByteArrayLiteral("tiff"), reader.format().toLower());
    const QImage storedImage = reader.read();
    ASSERT_FALSE(storedImage.isNull()) << reader.errorString().toStdString();
    EXPECT_EQ(sourceImage.size(), storedImage.size());
}

TEST_F(RasterExportTest, DownscalesToMaximumDimensionWithoutUpscaling)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QImage sourceImage(QSize(300, 200), QImage::Format_RGBA8888);
    sourceImage.fill(QColor(96, 112, 128));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("resized.png"));
    RasterExportOptions options;
    options.format = RasterExportFormat::Png;
    options.maximumDimension = 100;

    const RasterExportResult result = writeRasterImage(sourceImage, outputPath, options);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    const QImage storedImage(outputPath);
    ASSERT_FALSE(storedImage.isNull());
    EXPECT_LE(std::max(storedImage.width(), storedImage.height()), 100);
    EXPECT_EQ(storedImage.width() * 2, storedImage.height() * 3);
}

TEST_F(RasterExportTest, ConvertsAndEmbedsSelectedOutputColorSpace)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("display-p3.png"));
    RasterExportOptions options;
    options.format = RasterExportFormat::Png;
    options.outputColorSpace = RasterOutputColorSpace::DisplayP3;

    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    const QImage storedImage(outputPath);
    ASSERT_FALSE(storedImage.isNull());
    EXPECT_EQ(QColorSpace(QColorSpace::DisplayP3), storedImage.colorSpace());
}

TEST_F(RasterExportTest, CopiesSourceExifMetadataBeforeAtomicCommit)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.jpg"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.jpg"));
    ASSERT_TRUE(makeTestImage().save(sourcePath, "JPEG"));
    ASSERT_TRUE(writeExifArtist(sourcePath));

    RasterExportOptions options;
    options.format = RasterExportFormat::Jpeg;
    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options, sourcePath);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    EXPECT_EQ(QStringLiteral("Flexraw Test"), readExifArtist(outputPath));
}

TEST_F(RasterExportTest, NormalizesCopiedSourceExifOrientationForPhysicallyOrientedPixels)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.jpg"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.jpg"));
    ASSERT_TRUE(makeTestImage().save(sourcePath, "JPEG"));
    ASSERT_TRUE(writeExifOrientation(sourcePath, 8U));

    RasterExportOptions options;
    options.format = RasterExportFormat::Jpeg;
    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options, sourcePath);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    EXPECT_EQ(1U, readExifOrientation(outputPath));
}

TEST_F(RasterExportTest, NormalizesJpegAndTiffMetadataToFinalArtifact)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.jpg"));
    const QString thumbnailPath = QDir(directory.path()).filePath(QStringLiteral("thumbnail.jpg"));
    QImage sourceImage = makeTestImage();
    sourceImage.setColorSpace(QColorSpace::SRgb);
    ASSERT_TRUE(sourceImage.save(sourcePath, "JPEG"));
    ASSERT_TRUE(QImage(1, 1, QImage::Format_RGB32).save(thumbnailPath, "JPEG"));
    ASSERT_TRUE(writeArtifactExif(sourcePath, thumbnailPath));

    const std::array cases = {std::pair{RasterExportFormat::Jpeg, QStringLiteral("jpg")},
                              std::pair{RasterExportFormat::Tiff, QStringLiteral("tiff")}};
    for (const auto& [format, extension] : cases)
    {
        SCOPED_TRACE(extension.toStdString());
        const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.%1").arg(extension));
        RasterExportOptions options;
        options.format = format;
        options.outputColorSpace = RasterOutputColorSpace::DisplayP3;

        const RasterExportResult result = writeRasterImage(sourceImage, outputPath, options, sourcePath);

        ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
        EXPECT_EQ(QStringLiteral("Flexraw Test"), readExifArtist(outputPath));
        EXPECT_EQ(1U, readExifUnsigned(outputPath, "Exif.Image.Orientation"));
        EXPECT_EQ(3U, readExifUnsigned(outputPath, "Exif.Image.ImageWidth"));
        EXPECT_EQ(2U, readExifUnsigned(outputPath, "Exif.Image.ImageLength"));
        EXPECT_EQ(3U, readExifUnsigned(outputPath, "Exif.Photo.PixelXDimension"));
        EXPECT_EQ(2U, readExifUnsigned(outputPath, "Exif.Photo.PixelYDimension"));
        EXPECT_FALSE(hasExifThumbnail(outputPath));
        EXPECT_EQ(QColorSpace(QColorSpace::DisplayP3), QImage(outputPath).colorSpace());
    }
}

TEST_F(RasterExportTest, LeavesPngExifTransferUnsupported)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.jpg"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.png"));
    ASSERT_TRUE(makeTestImage().save(sourcePath, "JPEG"));
    ASSERT_TRUE(writeExifArtist(sourcePath));
    RasterExportOptions options;
    options.format = RasterExportFormat::Png;

    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options, sourcePath);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    EXPECT_TRUE(readExifArtist(outputPath).isEmpty());
}

TEST_F(RasterExportTest, ExcludesSourceExifMetadataWhenRequested)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.jpg"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.jpg"));
    ASSERT_TRUE(makeTestImage().save(sourcePath, "JPEG"));
    ASSERT_TRUE(writeExifArtist(sourcePath));

    RasterExportOptions options;
    options.format = RasterExportFormat::Jpeg;
    options.includeMetadata = false;
    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options, sourcePath);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    EXPECT_TRUE(readExifArtist(outputPath).isEmpty());
}

TEST_F(RasterExportTest, RejectsRasterSelfOverwriteAndPreservesSourceBytes)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.png"));
    ASSERT_TRUE(makeTestImage().save(sourcePath, "PNG"));
    QFile sourceFile(sourcePath);
    ASSERT_TRUE(sourceFile.open(QIODevice::ReadOnly));
    const QByteArray originalBytes = sourceFile.readAll();
    sourceFile.close();

    RasterExportOptions options;
    options.format = RasterExportFormat::Png;
    options.includeMetadata = false;
    const RasterExportResult result = writeRasterImage(makeTestImage(), sourcePath, options, sourcePath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, result.error().code);
    ASSERT_TRUE(sourceFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(originalBytes, sourceFile.readAll());
}

TEST_F(RasterExportTest, RejectsRawSelfOverwriteBeforeDecodeAndPreservesSourceBytes)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.raw"));
    QFile sourceFile(sourcePath);
    ASSERT_TRUE(sourceFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(12, sourceFile.write("raw-fixture!"));
    sourceFile.close();

    RasterExportOptions options;
    options.format = RasterExportFormat::Jpeg;
    const RasterExportResult result = writeRawImage(sourcePath, {}, sourcePath, options);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, result.error().code);
    ASSERT_TRUE(sourceFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(QByteArrayLiteral("raw-fixture!"), sourceFile.readAll());
}

TEST_F(RasterExportTest, ReplacesUnrelatedExistingOutputAtomically)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.png"));
    QFile outputFile(outputPath);
    ASSERT_TRUE(outputFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(8, outputFile.write("existing"));
    outputFile.close();

    RasterExportOptions options;
    options.format = RasterExportFormat::Png;
    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    const QImage storedImage(outputPath);
    ASSERT_FALSE(storedImage.isNull());
    EXPECT_EQ(makeTestImage().size(), storedImage.size());
}

TEST_F(RasterExportTest, IgnoresUnreadableMetadataSource)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("invalid-source.jpg"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.jpg"));
    QFile sourceFile(sourcePath);
    ASSERT_TRUE(sourceFile.open(QIODevice::WriteOnly));
    ASSERT_EQ(7, sourceFile.write("invalid"));
    sourceFile.close();
    RasterExportOptions options;
    options.format = RasterExportFormat::Jpeg;
    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options, sourcePath);

    ASSERT_TRUE(result.hasValue()) << result.error().message.toStdString();
    EXPECT_TRUE(QFileInfo::exists(outputPath));
}

TEST_F(RasterExportTest, LeavesNoFinalArtifactWhenMetadataWriteFails)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.tiff"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.jpg"));
    ASSERT_TRUE(makeTestImage().save(sourcePath, "TIFF"));
    ASSERT_TRUE(writeOversizedExif(sourcePath));
    RasterExportOptions options;
    options.format = RasterExportFormat::Jpeg;

    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options, sourcePath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DecodeFailed, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(outputPath));
}

TEST_F(RasterExportTest, RejectsNullImage)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("image.png"));
    RasterExportOptions options;
    options.format = RasterExportFormat::Png;

    const RasterExportResult result = writeRasterImage(QImage(), outputPath, options);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(outputPath));
}

TEST_F(RasterExportTest, PreservesExistingOutputWhenCancellationIsAlreadyRequested)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("image.png"));
    QFile existingOutput(outputPath);
    ASSERT_TRUE(existingOutput.open(QIODevice::WriteOnly));
    ASSERT_EQ(8, existingOutput.write("existing"));
    existingOutput.close();
    types::CancellationSource cancellationSource;
    cancellationSource.requestCancellation();
    const types::CancellationToken cancellationToken = cancellationSource.token();
    RasterExportOptions options;
    options.format = RasterExportFormat::Png;

    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options, {}, &cancellationToken);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::Cancelled, result.error().code);
    ASSERT_TRUE(existingOutput.open(QIODevice::ReadOnly));
    EXPECT_EQ(QByteArrayLiteral("existing"), existingOutput.readAll());
}

TEST_F(RasterExportTest, RejectsMissingRawInput)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    RasterExportOptions options;
    options.format = RasterExportFormat::Png;

    const RasterExportResult result = writeRawImage(QDir(directory.path()).filePath(QStringLiteral("missing.raw")),
                                                    {},
                                                    QDir(directory.path()).filePath(QStringLiteral("output.png")),
                                                    options);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

TEST_F(RasterExportTest, RejectsInvalidRawOptionsBeforeDecode)
{
    RasterExportOptions options;
    options.jpegQuality = 0;

    const RasterExportResult result =
        writeRawImage(QStringLiteral("missing.cr3"), {}, QStringLiteral("unused.jpg"), options);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST_F(RasterExportTest, RejectsInvalidFormatOptions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QImage sourceImage = makeTestImage();

    RasterExportOptions jpegOptions;
    jpegOptions.format = RasterExportFormat::Jpeg;
    jpegOptions.jpegQuality = 0;
    const RasterExportResult invalidJpeg =
        writeRasterImage(sourceImage, QDir(directory.path()).filePath(QStringLiteral("image.jpg")), jpegOptions);

    RasterExportOptions pngOptions;
    pngOptions.format = RasterExportFormat::Png;
    pngOptions.pngCompression = 10;
    const RasterExportResult invalidPng =
        writeRasterImage(sourceImage, QDir(directory.path()).filePath(QStringLiteral("image.png")), pngOptions);

    RasterExportOptions tiffOptions;
    tiffOptions.format = RasterExportFormat::Tiff;
    tiffOptions.tiffCompression = static_cast<TiffCompression>(std::numeric_limits<int>::max());
    const RasterExportResult invalidTiff =
        writeRasterImage(sourceImage, QDir(directory.path()).filePath(QStringLiteral("image.tiff")), tiffOptions);

    RasterExportOptions resizeOptions;
    resizeOptions.format = RasterExportFormat::Png;
    resizeOptions.maximumDimension = -1;
    const RasterExportResult invalidResize =
        writeRasterImage(sourceImage, QDir(directory.path()).filePath(QStringLiteral("resize.png")), resizeOptions);

    RasterExportOptions colorSpaceOptions;
    colorSpaceOptions.format = RasterExportFormat::Png;
    colorSpaceOptions.outputColorSpace = static_cast<RasterOutputColorSpace>(std::numeric_limits<int>::max());
    const RasterExportResult invalidColorSpace = writeRasterImage(
        sourceImage, QDir(directory.path()).filePath(QStringLiteral("color-space.png")), colorSpaceOptions);

    ASSERT_TRUE(invalidJpeg.hasError());
    ASSERT_TRUE(invalidPng.hasError());
    ASSERT_TRUE(invalidTiff.hasError());
    ASSERT_TRUE(invalidResize.hasError());
    ASSERT_TRUE(invalidColorSpace.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidJpeg.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidPng.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidTiff.error().code);
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidResize.error().code);
    EXPECT_EQ(types::ErrorCode::UnsupportedFormat, invalidColorSpace.error().code);
}

TEST_F(RasterExportTest, RejectsUnsupportedFormat)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    RasterExportOptions options;
    options.format = static_cast<RasterExportFormat>(std::numeric_limits<int>::max());
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("image.data"));

    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::UnsupportedFormat, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(outputPath));
}

TEST_F(RasterExportTest, RejectsMissingParentDirectory)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    RasterExportOptions options;
    options.format = RasterExportFormat::Png;
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("missing/parent/image.png"));

    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(outputPath));
}

TEST_F(RasterExportTest, ReportsWriteFailureWithoutPartialOutput)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());

    RasterExportOptions options;
    options.format = RasterExportFormat::Png;
    const QString fileName(300, QLatin1Char('x'));
    const QString outputPath = QDir(directory.path()).filePath(fileName + QStringLiteral(".png"));

    const RasterExportResult result = writeRasterImage(makeTestImage(), outputPath, options);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::PermissionDenied, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(outputPath));
    EXPECT_TRUE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).isEmpty());
}

}  // namespace
}  // namespace flexraw::core::export_

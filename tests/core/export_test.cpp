#include <algorithm>
#include <cstring>
#include <limits>

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

#include <algorithm>

#include <QColor>
#include <QColorSpace>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "export_command_service.h"

namespace flexraw::ui::cli
{
namespace
{

// 목적: raster export command test에 사용할 색상 profile이 포함된 입력 image 저장
// 입력: path: 저장할 PNG file 경로
// 출력: image 저장 성공 여부
[[nodiscard]] bool writeInputImage(const QString& path)
{
    QImage image(QSize(30, 20), QImage::Format_RGBA8888);
    image.fill(QColor(64, 128, 192, 255));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    return image.save(path, "PNG");
}

TEST(ExportCommandServiceTest, ParsesRasterExportOptions)
{
    const RasterExportCommandParseResult parsed = ExportCommandService::parseRasterCommand(QStringLiteral(
        "export raster --input \"C:\\Photo Folder\\input.png\" --output \"D:\\Output\\image.png\" "
        "--format png --color-space display-p3 --compression 9 --max-dimension 1200 --metadata exclude"));

    ASSERT_TRUE(parsed.recognized);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(QStringLiteral("C:\\Photo Folder\\input.png"), parsed.command.inputPath);
    EXPECT_EQ(QStringLiteral("D:\\Output\\image.png"), parsed.command.outputPath);
    EXPECT_EQ(core::export_::RasterExportFormat::Png, parsed.command.options.format);
    EXPECT_EQ(core::export_::RasterOutputColorSpace::DisplayP3, parsed.command.options.outputColorSpace);
    EXPECT_EQ(9, parsed.command.options.pngCompression);
    EXPECT_EQ(1200, parsed.command.options.maximumDimension);
    EXPECT_FALSE(parsed.command.options.includeMetadata);
}

TEST(ExportCommandServiceTest, RejectsIncompleteAndUnknownOptions)
{
    const RasterExportCommandParseResult missing =
        ExportCommandService::parseRasterCommand(QStringLiteral("export raster --input image.png --format png"));
    const RasterExportCommandParseResult unknown = ExportCommandService::parseRasterCommand(
        QStringLiteral("export raster --input image.png --output output.png --format png --source raw"));
    const RasterExportCommandParseResult invalidMetadata = ExportCommandService::parseRasterCommand(
        QStringLiteral("export raster --input image.png --output output.png --format png --metadata retain"));

    EXPECT_TRUE(missing.recognized);
    EXPECT_FALSE(missing.valid);
    EXPECT_TRUE(unknown.recognized);
    EXPECT_FALSE(unknown.valid);
    EXPECT_TRUE(invalidMetadata.recognized);
    EXPECT_FALSE(invalidMetadata.valid);
}

TEST(ExportCommandServiceTest, ParsesRawExportOptions)
{
    const RasterExportCommandParseResult parsed = ExportCommandService::parseRawCommand(QStringLiteral(
        "export raw --input \"D:\\Photos\\input.raw\" --output \"D:\\Output\\image.tiff\" "
        "--format tiff --color-space adobe-rgb --tiff-compression none --catalog \"D:\\Catalog\\library.db\""));

    ASSERT_TRUE(parsed.recognized);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(QStringLiteral("D:\\Photos\\input.raw"), parsed.command.inputPath);
    EXPECT_EQ(core::export_::RasterExportFormat::Tiff, parsed.command.options.format);
    EXPECT_EQ(core::export_::RasterOutputColorSpace::AdobeRgb, parsed.command.options.outputColorSpace);
    EXPECT_EQ(core::export_::TiffCompression::None, parsed.command.options.tiffCompression);
    EXPECT_EQ(QStringLiteral("D:\\Catalog\\library.db"), parsed.command.catalogPath);
}

TEST(ExportCommandServiceTest, JoinsLineWrappedQuotedPath)
{
    const RasterExportCommandParseResult parsed = ExportCommandService::parseRawCommand(QStringLiteral(
        "export raw --input \"D:\\Photos\\input.raw\" --output \"D:\\Output\\very-long-sample-\n"
        "  oriented.jpg\" --format jpeg"));

    ASSERT_TRUE(parsed.recognized);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(QStringLiteral("D:\\Output\\very-long-sample-oriented.jpg"), parsed.command.outputPath);
}

TEST(ExportCommandServiceTest, ParsesBatchExportOptions)
{
    const BatchExportCommandParseResult parsed = ExportCommandService::parseBatchCommand(
        QStringLiteral("export batch --input-folder \"D:\\Photos\" --output-folder \"D:\\Output\" --format jpeg "
                       "--workers 4 --color-space srgb --quality 90 --metadata exclude"));

    ASSERT_TRUE(parsed.recognized);
    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(QStringLiteral("D:\\Photos"), parsed.command.inputFolderPath);
    EXPECT_EQ(QStringLiteral("D:\\Output"), parsed.command.outputFolderPath);
    EXPECT_EQ(4, parsed.command.workerCount);
    EXPECT_EQ(core::export_::RasterExportFormat::Jpeg, parsed.command.options.format);
    EXPECT_EQ(90, parsed.command.options.jpegQuality);
    EXPECT_FALSE(parsed.command.options.includeMetadata);
}

TEST(ExportCommandServiceTest, AppliesStoredDefaultsBeforeExplicitOptions)
{
    core::export_::RasterExportOptions defaults;
    defaults.jpegQuality = 76;
    defaults.pngCompression = 3;
    defaults.maximumDimension = 1600;
    defaults.outputColorSpace = core::export_::RasterOutputColorSpace::AdobeRgb;

    const RasterExportCommandParseResult parsed = ExportCommandService::parseRasterCommand(
        QStringLiteral("export raster --input input.png --output output.png --format jpeg"), defaults);

    ASSERT_TRUE(parsed.valid);
    EXPECT_EQ(76, parsed.command.options.jpegQuality);
    EXPECT_EQ(3, parsed.command.options.pngCompression);
    EXPECT_EQ(1600, parsed.command.options.maximumDimension);
    EXPECT_EQ(core::export_::RasterOutputColorSpace::AdobeRgb, parsed.command.options.outputColorSpace);
}

TEST(ExportCommandServiceTest, ExportsRasterInputWithRequestedOptions)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString inputPath = QDir(directory.path()).filePath(QStringLiteral("input.png"));
    const QString outputPath = QDir(directory.path()).filePath(QStringLiteral("output.png"));
    ASSERT_TRUE(writeInputImage(inputPath));

    RasterExportCommand command;
    command.inputPath = inputPath;
    command.outputPath = outputPath;
    command.options.format = core::export_::RasterExportFormat::Png;
    command.options.outputColorSpace = core::export_::RasterOutputColorSpace::DisplayP3;
    command.options.maximumDimension = 10;

    const RasterExportCommandExecutionResult result = ExportCommandService::exportRaster(command);

    ASSERT_TRUE(result.succeeded) << result.errorMessage.toStdString();
    const QImage outputImage(outputPath);
    ASSERT_FALSE(outputImage.isNull());
    EXPECT_LE(std::max(outputImage.width(), outputImage.height()), 10);
    EXPECT_EQ(QColorSpace(QColorSpace::DisplayP3), outputImage.colorSpace());
}

TEST(ExportCommandServiceTest, RejectsCatalogOptionForRasterExport)
{
    RasterExportCommand command;
    command.inputPath = QStringLiteral("input.png");
    command.outputPath = QStringLiteral("output.png");
    command.catalogPath = QStringLiteral("library.flexraw-catalog");

    const RasterExportCommandExecutionResult result = ExportCommandService::exportRaster(command);

    EXPECT_FALSE(result.succeeded);
    EXPECT_TRUE(result.errorMessage.contains(QStringLiteral("--catalog")));
}

TEST(ExportCommandServiceTest, ExportsRasterFolderInParallel)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString inputFolderPath = QDir(directory.path()).filePath(QStringLiteral("input"));
    const QString outputFolderPath = QDir(directory.path()).filePath(QStringLiteral("output"));
    ASSERT_TRUE(QDir().mkpath(inputFolderPath));
    ASSERT_TRUE(QDir().mkpath(outputFolderPath));
    ASSERT_TRUE(writeInputImage(QDir(inputFolderPath).filePath(QStringLiteral("first.png"))));
    ASSERT_TRUE(writeInputImage(QDir(inputFolderPath).filePath(QStringLiteral("second.png"))));

    BatchExportCommand command;
    command.inputFolderPath = inputFolderPath;
    command.outputFolderPath = outputFolderPath;
    command.workerCount = 2;
    command.options.format = core::export_::RasterExportFormat::Png;
    command.options.maximumDimension = 10;

    const BatchExportCommandExecutionResult result = ExportCommandService::exportBatch(command);

    ASSERT_TRUE(result.completed) << result.firstError.toStdString();
    EXPECT_EQ(2, result.totalCount);
    EXPECT_EQ(2, result.succeededCount);
    EXPECT_EQ(0, result.failedCount);
    EXPECT_TRUE(QFile::exists(QDir(outputFolderPath).filePath(QStringLiteral("first-png.png"))));
    EXPECT_TRUE(QFile::exists(QDir(outputFolderPath).filePath(QStringLiteral("second-png.png"))));
}

}  // namespace
}  // namespace flexraw::ui::cli

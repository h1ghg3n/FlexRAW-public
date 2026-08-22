#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>

#include "export_settings.h"

namespace flexraw::ui::settings
{
namespace
{

TEST(ExportSettingsTest, PersistsAndRestoresRasterDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    ExportSettings exportSettings(settings);

    core::export_::RasterExportOptions saved;
    saved.format = core::export_::RasterExportFormat::Tiff;
    saved.jpegQuality = 82;
    saved.pngCompression = 2;
    saved.tiffCompression = core::export_::TiffCompression::None;
    saved.maximumDimension = 2400;
    saved.outputColorSpace = core::export_::RasterOutputColorSpace::AdobeRgb;
    saved.includeMetadata = false;
    exportSettings.saveRasterDefaults(saved);

    const core::export_::RasterExportOptions restored = exportSettings.loadRasterDefaults();

    EXPECT_EQ(saved.format, restored.format);
    EXPECT_EQ(saved.jpegQuality, restored.jpegQuality);
    EXPECT_EQ(saved.pngCompression, restored.pngCompression);
    EXPECT_EQ(saved.tiffCompression, restored.tiffCompression);
    EXPECT_EQ(saved.maximumDimension, restored.maximumDimension);
    EXPECT_EQ(saved.outputColorSpace, restored.outputColorSpace);
    EXPECT_EQ(saved.includeMetadata, restored.includeMetadata);
}

TEST(ExportSettingsTest, FallsBackForInvalidStoredValues)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("export"));
    settings.setValue(QStringLiteral("jpegQuality"), 101);
    settings.setValue(QStringLiteral("pngCompression"), -1);
    settings.setValue(QStringLiteral("maximumDimension"), -10);
    settings.setValue(QStringLiteral("outputColorSpace"), 99);
    settings.endGroup();
    ExportSettings exportSettings(settings);

    const core::export_::RasterExportOptions restored = exportSettings.loadRasterDefaults();

    EXPECT_EQ(90, restored.jpegQuality);
    EXPECT_EQ(6, restored.pngCompression);
    EXPECT_EQ(0, restored.maximumDimension);
    EXPECT_EQ(core::export_::RasterOutputColorSpace::Srgb, restored.outputColorSpace);
}

TEST(ExportSettingsTest, PersistsAndRestoresExecutionDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    ExportSettings exportSettings(settings);
    const QString sourceStorageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString outputStorageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const ExportExecutionDefaults saved{
        ExportExecutionMode::Remote,
        QStringLiteral("192.0.2.42"),
        49200,
        sourceStorageId,
        outputStorageId,
    };

    exportSettings.saveExecutionDefaults(saved);
    const ExportExecutionDefaults restored = exportSettings.loadExecutionDefaults();

    EXPECT_EQ(saved.mode, restored.mode);
    EXPECT_EQ(saved.remoteHost, restored.remoteHost);
    EXPECT_EQ(saved.remotePort, restored.remotePort);
    EXPECT_EQ(saved.expectedSourceStorageId, restored.expectedSourceStorageId);
    EXPECT_EQ(saved.expectedOutputStorageId, restored.expectedOutputStorageId);
}

TEST(ExportSettingsTest, FallsBackForInvalidExecutionDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("export"));
    settings.setValue(QStringLiteral("executionMode"), 99);
    settings.setValue(QStringLiteral("remoteHost"), QStringLiteral("   "));
    settings.setValue(QStringLiteral("remotePort"), 70000);
    settings.setValue(QStringLiteral("expectedSourceStorageId"), QStringLiteral("not-a-uuid"));
    settings.setValue(QStringLiteral("expectedOutputStorageId"),
                      QStringLiteral("00000000-0000-0000-0000-000000000000"));
    settings.endGroup();
    ExportSettings exportSettings(settings);

    const ExportExecutionDefaults restored = exportSettings.loadExecutionDefaults();

    EXPECT_EQ(ExportExecutionMode::Local, restored.mode);
    EXPECT_EQ(QStringLiteral("127.0.0.1"), restored.remoteHost);
    EXPECT_EQ(47331, restored.remotePort);
    EXPECT_TRUE(restored.expectedSourceStorageId.isEmpty());
    EXPECT_TRUE(restored.expectedOutputStorageId.isEmpty());
}

TEST(ExportSettingsTest, PersistsAndRestoresAutoExecutionMode)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    ExportSettings exportSettings(settings);
    ExportExecutionDefaults saved;
    saved.mode = ExportExecutionMode::Auto;

    exportSettings.saveExecutionDefaults(saved);

    EXPECT_EQ(ExportExecutionMode::Auto, exportSettings.loadExecutionDefaults().mode);
}

TEST(ExportSettingsTest, RemovesLegacyManualRootKeysWhenLoadingProfile)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("export"));
    settings.setValue(QStringLiteral("localSourceRoot"), QStringLiteral("Z:/shared-photos"));
    settings.setValue(QStringLiteral("localOutputRoot"), QStringLiteral("Z:/shared-exports"));
    settings.endGroup();
    ExportSettings exportSettings(settings);

    (void)exportSettings.loadExecutionDefaults();

    settings.beginGroup(QStringLiteral("export"));
    EXPECT_FALSE(settings.contains(QStringLiteral("localSourceRoot")));
    EXPECT_FALSE(settings.contains(QStringLiteral("localOutputRoot")));
    settings.endGroup();
}

}  // namespace
}  // namespace flexraw::ui::settings

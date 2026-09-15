#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>

#include "qt_export_settings_adapter.h"

namespace flexraw::ui::settings
{
namespace
{

TEST(QtExportSettingsAdapterTest, PersistsAndRestoresRasterDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtExportSettingsAdapter adapter(settings);

    core::client::ExportRasterOptions saved;
    saved.format = core::client::ExportRasterFormat::Tiff;
    saved.jpegQuality = 82;
    saved.pngCompression = 2;
    saved.tiffCompression = core::client::ExportTiffCompression::None;
    saved.maximumDimension = 2400;
    saved.outputColorSpace = core::client::ExportOutputColorSpace::AdobeRgb;
    saved.includeMetadata = false;
    ASSERT_TRUE(adapter.saveRasterExportDefaults(saved).hasValue());

    const core::client::ExportDefaultsResult restored = adapter.exportDefaults();

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(saved, restored.value().rasterOptions);
}

TEST(QtExportSettingsAdapterTest, FallsBackForInvalidStoredValues)
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
    QtExportSettingsAdapter adapter(settings);

    const core::client::ExportDefaultsResult restored = adapter.exportDefaults();

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(90, restored.value().rasterOptions.jpegQuality);
    EXPECT_EQ(6, restored.value().rasterOptions.pngCompression);
    EXPECT_EQ(0, restored.value().rasterOptions.maximumDimension);
    EXPECT_EQ(core::client::ExportOutputColorSpace::Srgb, restored.value().rasterOptions.outputColorSpace);
}

TEST(QtExportSettingsAdapterTest, PersistsAndRestoresExecutionDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtExportSettingsAdapter adapter(settings);
    const core::client::WorkerProfileId profileId{QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString()};
    const core::client::ExportExecutionDefaults saved{
        core::client::ExportPlacementPolicy::RemoteOnly,
        profileId,
    };

    ASSERT_TRUE(adapter.saveExportExecutionDefaults(saved).hasValue());
    const core::client::ExportDefaultsResult restored = adapter.exportDefaults();

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(saved, restored.value().execution);
}

TEST(QtExportSettingsAdapterTest, FallsBackForInvalidExecutionDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("export"));
    settings.setValue(QStringLiteral("executionMode"), 99);
    settings.setValue(QStringLiteral("preferredWorkerProfileId"), QStringLiteral("not-a-uuid"));
    settings.endGroup();
    QtExportSettingsAdapter adapter(settings);

    const core::client::ExportDefaultsResult restored = adapter.exportDefaults();

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::ExportPlacementPolicy::LocalOnly, restored.value().execution.placementPolicy);
    EXPECT_FALSE(restored.value().execution.preferredWorkerProfileId.has_value());
}

TEST(QtExportSettingsAdapterTest, PersistsAndRestoresAutoExecutionMode)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtExportSettingsAdapter adapter(settings);
    core::client::ExportExecutionDefaults saved;
    saved.placementPolicy = core::client::ExportPlacementPolicy::Auto;

    ASSERT_TRUE(adapter.saveExportExecutionDefaults(saved).hasValue());
    const core::client::ExportDefaultsResult restored = adapter.exportDefaults();

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::ExportPlacementPolicy::Auto, restored.value().execution.placementPolicy);
}

TEST(QtExportSettingsAdapterTest, RejectsInvalidRasterSaveWithoutMutatingStoredDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtExportSettingsAdapter adapter(settings);
    core::client::ExportRasterOptions invalid;
    invalid.jpegQuality = 101;

    const core::client::ExportRasterDefaultsResult saved = adapter.saveRasterExportDefaults(invalid);
    const core::client::ExportDefaultsResult restored = adapter.exportDefaults();

    ASSERT_TRUE(saved.hasError());
    EXPECT_EQ(core::client::ClientErrorCode::InvalidArgument, saved.error().code);
    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::ExportRasterOptions{}, restored.value().rasterOptions);
}

TEST(QtExportSettingsAdapterTest, RejectsInvalidWorkerIdentityWithoutMutatingStoredDefaults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings settings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtExportSettingsAdapter adapter(settings);
    const core::client::ExportExecutionDefaults invalid{
        core::client::ExportPlacementPolicy::RemoteOnly,
        core::client::WorkerProfileId{"not-a-uuid"},
    };

    const core::client::ExportExecutionDefaultsResult saved = adapter.saveExportExecutionDefaults(invalid);
    const core::client::ExportDefaultsResult restored = adapter.exportDefaults();

    ASSERT_TRUE(saved.hasError());
    EXPECT_EQ(core::client::ClientErrorCode::InvalidArgument, saved.error().code);
    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::ExportExecutionDefaults{}, restored.value().execution);
}

}  // namespace
}  // namespace flexraw::ui::settings

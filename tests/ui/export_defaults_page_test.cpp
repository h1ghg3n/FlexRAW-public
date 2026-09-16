#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "qt_export_settings_adapter.h"
#include "qt_worker_profile_settings_adapter.h"
#include "settings_dialog.h"

namespace flexraw::ui::settings
{
namespace
{

TEST(ExportDefaultsPageTest, LoadsAndSavesDefaultsThroughProductClients)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    const core::client::WorkerProfileResult profile =
        workerProfiles.createWorkerProfile({"Remote Worker", "192.168.1.90", 47331, true, {}, {}});
    ASSERT_TRUE(profile.hasValue());
    core::client::ExportDefaultsSnapshot initial;
    initial.rasterOptions.format = core::client::ExportRasterFormat::Tiff;
    initial.rasterOptions.jpegQuality = 82;
    initial.rasterOptions.pngCompression = 2;
    initial.rasterOptions.tiffCompression = core::client::ExportTiffCompression::None;
    initial.rasterOptions.maximumDimension = 2400;
    initial.rasterOptions.outputColorSpace = core::client::ExportOutputColorSpace::AdobeRgb;
    initial.rasterOptions.includeMetadata = false;
    initial.execution = {core::client::ExportPlacementPolicy::RemoteOnly, profile.value().id};
    ASSERT_TRUE(exportDefaults.saveRasterExportDefaults(initial.rasterOptions).hasValue());
    ASSERT_TRUE(exportDefaults.saveExportExecutionDefaults(initial.execution).hasValue());

    SettingsDialog dialog(storage, workerProfiles, exportDefaults);
    auto* format = dialog.findChild<QComboBox*>(QStringLiteral("exportDefaultsFormatComboBox"));
    auto* quality = dialog.findChild<QSpinBox*>(QStringLiteral("exportDefaultsJpegQualitySpinBox"));
    auto* placement = dialog.findChild<QComboBox*>(QStringLiteral("exportDefaultsPlacementComboBox"));
    auto* preferred = dialog.findChild<QComboBox*>(QStringLiteral("exportDefaultsWorkerProfileComboBox"));
    auto* metadata = dialog.findChild<QCheckBox*>(QStringLiteral("exportDefaultsMetadataCheckBox"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(nullptr, format);
    ASSERT_NE(nullptr, quality);
    ASSERT_NE(nullptr, placement);
    ASSERT_NE(nullptr, preferred);
    ASSERT_NE(nullptr, metadata);
    ASSERT_NE(nullptr, buttons);
    EXPECT_EQ(static_cast<int>(core::client::ExportRasterFormat::Tiff), format->currentData().toInt());
    EXPECT_EQ(82, quality->value());
    EXPECT_EQ(static_cast<int>(core::client::ExportPlacementPolicy::RemoteOnly), placement->currentData().toInt());
    EXPECT_EQ(QString::fromStdString(profile.value().id.value), preferred->currentData().toString());
    EXPECT_FALSE(metadata->isChecked());

    format->setCurrentIndex(format->findData(static_cast<int>(core::client::ExportRasterFormat::Png)));
    quality->setValue(77);
    placement->setCurrentIndex(placement->findData(static_cast<int>(core::client::ExportPlacementPolicy::Auto)));
    metadata->setChecked(true);
    buttons->button(QDialogButtonBox::Ok)->click();

    ASSERT_EQ(QDialog::Accepted, dialog.result());
    const core::client::ExportDefaultsResult restored = exportDefaults.exportDefaults();
    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::ExportRasterFormat::Png, restored.value().rasterOptions.format);
    EXPECT_EQ(77, restored.value().rasterOptions.jpegQuality);
    EXPECT_TRUE(restored.value().rasterOptions.includeMetadata);
    EXPECT_EQ(core::client::ExportPlacementPolicy::Auto, restored.value().execution.placementPolicy);
    ASSERT_TRUE(restored.value().execution.preferredWorkerProfileId.has_value());
    EXPECT_EQ(profile.value().id, *restored.value().execution.preferredWorkerProfileId);
}

TEST(ExportDefaultsPageTest, CancelLeavesPersistedDefaultsUnchanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    ASSERT_TRUE(workerProfiles.listWorkerProfiles().hasValue());
    const core::client::ExportDefaultsResult before = exportDefaults.exportDefaults();
    ASSERT_TRUE(before.hasValue());

    SettingsDialog dialog(storage, workerProfiles, exportDefaults);
    auto* quality = dialog.findChild<QSpinBox*>(QStringLiteral("exportDefaultsJpegQualitySpinBox"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(nullptr, quality);
    ASSERT_NE(nullptr, buttons);
    quality->setValue(55);
    buttons->button(QDialogButtonBox::Cancel)->click();

    EXPECT_EQ(QDialog::Rejected, dialog.result());
    const core::client::ExportDefaultsResult after = exportDefaults.exportDefaults();
    ASSERT_TRUE(after.hasValue());
    EXPECT_EQ(before.value(), after.value());
}

TEST(ExportDefaultsPageTest, RefreshesPreferredWorkerAfterProfileMutation)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    SettingsDialog dialog(storage, workerProfiles, exportDefaults);
    const core::client::WorkerProfileResult profile =
        workerProfiles.createWorkerProfile({"Remote Worker", "192.168.1.90", 47331, true, {}, {}});
    ASSERT_TRUE(profile.hasValue());

    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
    auto* preferred = dialog.findChild<QComboBox*>(QStringLiteral("exportDefaultsWorkerProfileComboBox"));
    ASSERT_NE(nullptr, tabs);
    ASSERT_NE(nullptr, preferred);
    tabs->setCurrentIndex(tabs->count() - 1);

    EXPECT_GE(preferred->findData(QString::fromStdString(profile.value().id.value)), 0);
}

TEST(ExportDefaultsPageTest, KeepsExplicitNoPreferenceAcrossProfileRefresh)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    const core::client::WorkerProfileResult profile =
        workerProfiles.createWorkerProfile({"Remote Worker", "192.168.1.90", 47331, true, {}, {}});
    ASSERT_TRUE(profile.hasValue());
    ASSERT_TRUE(
        exportDefaults.saveExportExecutionDefaults({core::client::ExportPlacementPolicy::Auto, profile.value().id})
            .hasValue());
    SettingsDialog dialog(storage, workerProfiles, exportDefaults);

    auto* tabs = dialog.findChild<QTabWidget*>(QStringLiteral("settingsTabs"));
    auto* preferred = dialog.findChild<QComboBox*>(QStringLiteral("exportDefaultsWorkerProfileComboBox"));
    ASSERT_NE(nullptr, tabs);
    ASSERT_NE(nullptr, preferred);
    preferred->setCurrentIndex(0);
    tabs->setCurrentIndex(1);
    tabs->setCurrentIndex(tabs->count() - 1);

    EXPECT_TRUE(preferred->currentData().toString().isEmpty());
}

}  // namespace
}  // namespace flexraw::ui::settings

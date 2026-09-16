#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "qt_catalog_startup_settings_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_worker_profile_settings_adapter.h"
#include "settings_dialog.h"

namespace flexraw::ui::settings
{
namespace
{

TEST(QtCatalogStartupSettingsAdapterTest, DefaultsToReopenLastActiveAndPersistsManagedCatalogChoice)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtCatalogStartupSettingsAdapter adapter(storage);

    const core::client::CatalogStartupSettingsResult initial = adapter.catalogStartupSettings();
    ASSERT_TRUE(initial.hasValue());
    EXPECT_EQ(core::client::CatalogStartupBehavior::ReopenLastActive, initial.value().behavior);
    EXPECT_FALSE(initial.value().managedCatalogPath.empty());

    const core::client::CatalogStartupSettingsResult saved =
        adapter.saveCatalogStartupBehavior(core::client::CatalogStartupBehavior::OpenManagedCatalog);
    ASSERT_TRUE(saved.hasValue());
    EXPECT_EQ(core::client::CatalogStartupBehavior::OpenManagedCatalog, saved.value().behavior);

    storage.beginGroup(QStringLiteral("catalog"));
    EXPECT_EQ(QStringLiteral("open-managed-catalog"), storage.value(QStringLiteral("startupBehavior")).toString());
    storage.endGroup();
}

TEST(QtCatalogStartupSettingsAdapterTest, InvalidStoredBehaviorFallsBackToExistingStartupPolicy)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    storage.setValue(QStringLiteral("catalog/startupBehavior"), QStringLiteral("future-policy"));
    QtCatalogStartupSettingsAdapter adapter(storage);

    const core::client::CatalogStartupSettingsResult restored = adapter.catalogStartupSettings();

    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::CatalogStartupBehavior::ReopenLastActive, restored.value().behavior);
}

TEST(QtCatalogStartupSettingsAdapterTest, RejectsUnknownBehaviorWithoutMutatingStoredPolicy)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtCatalogStartupSettingsAdapter adapter(storage);

    const core::client::CatalogStartupSettingsResult saved =
        adapter.saveCatalogStartupBehavior(static_cast<core::client::CatalogStartupBehavior>(99));

    ASSERT_TRUE(saved.hasError());
    EXPECT_EQ(core::client::ClientErrorCode::InvalidArgument, saved.error().code);
    EXPECT_FALSE(storage.contains(QStringLiteral("catalog/startupBehavior")));
}

TEST(GeneralSettingsPageTest, SavesStartupPolicyOnlyWhenDialogIsAccepted)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtCatalogStartupSettingsAdapter startupSettings(storage);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    SettingsDialog dialog(storage, workerProfiles, exportDefaults, nullptr, nullptr, &startupSettings);
    auto* behavior = dialog.findChild<QComboBox*>(QStringLiteral("catalogStartupBehaviorComboBox"));
    auto* managedPath = dialog.findChild<QLineEdit*>(QStringLiteral("managedCatalogPathEdit"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(nullptr, behavior);
    ASSERT_NE(nullptr, managedPath);
    ASSERT_NE(nullptr, buttons);
    EXPECT_TRUE(managedPath->isReadOnly());
    EXPECT_FALSE(managedPath->text().isEmpty());

    behavior->setCurrentIndex(
        behavior->findData(static_cast<int>(core::client::CatalogStartupBehavior::OpenManagedCatalog)));
    buttons->button(QDialogButtonBox::Ok)->click();

    ASSERT_EQ(QDialog::Accepted, dialog.result());
    const core::client::CatalogStartupSettingsResult saved = startupSettings.catalogStartupSettings();
    ASSERT_TRUE(saved.hasValue());
    EXPECT_EQ(core::client::CatalogStartupBehavior::OpenManagedCatalog, saved.value().behavior);
}

TEST(GeneralSettingsPageTest, CancelKeepsStartupPolicyUnchanged)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtCatalogStartupSettingsAdapter startupSettings(storage);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    SettingsDialog dialog(storage, workerProfiles, exportDefaults, nullptr, nullptr, &startupSettings);
    auto* behavior = dialog.findChild<QComboBox*>(QStringLiteral("catalogStartupBehaviorComboBox"));
    auto* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(nullptr, behavior);
    ASSERT_NE(nullptr, buttons);

    behavior->setCurrentIndex(
        behavior->findData(static_cast<int>(core::client::CatalogStartupBehavior::OpenManagedCatalog)));
    buttons->button(QDialogButtonBox::Cancel)->click();

    ASSERT_EQ(QDialog::Rejected, dialog.result());
    const core::client::CatalogStartupSettingsResult restored = startupSettings.catalogStartupSettings();
    ASSERT_TRUE(restored.hasValue());
    EXPECT_EQ(core::client::CatalogStartupBehavior::ReopenLastActive, restored.value().behavior);
}

}  // namespace
}  // namespace flexraw::ui::settings

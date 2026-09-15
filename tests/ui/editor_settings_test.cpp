#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "editor_settings.h"
#include "qt_export_settings_adapter.h"
#include "qt_worker_profile_settings_adapter.h"
#include "settings_dialog.h"

namespace flexraw::ui::settings
{
namespace
{

TEST(EditorSettingsTest, DefaultsToClassicAndPersistsRelativeStyle)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    EditorSettings settings(storage);

    EXPECT_EQ(editor::AdjustmentControlStyle::Classic, settings.loadAdjustmentControlStyle());

    settings.saveAdjustmentControlStyle(editor::AdjustmentControlStyle::Relative);

    EXPECT_EQ(editor::AdjustmentControlStyle::Relative, settings.loadAdjustmentControlStyle());
}

TEST(EditorSettingsTest, InvalidStoredStyleFallsBackToClassic)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    storage.setValue(QStringLiteral("ui/adjustmentControlStyle"), 99);

    EXPECT_EQ(editor::AdjustmentControlStyle::Classic, EditorSettings(storage).loadAdjustmentControlStyle());
}

TEST(SettingsDialogTest, LoadsAndSavesAcceptedAdjustmentStyle)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    EditorSettings(storage).saveAdjustmentControlStyle(editor::AdjustmentControlStyle::Relative);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    SettingsDialog dialog(storage, workerProfiles, exportDefaults);
    QComboBox* styleCombo = dialog.findChild<QComboBox*>(QStringLiteral("adjustmentControlStyleComboBox"));
    QDialogButtonBox* buttons = dialog.findChild<QDialogButtonBox*>();
    ASSERT_NE(nullptr, styleCombo);
    ASSERT_NE(nullptr, buttons);
    EXPECT_EQ(static_cast<int>(editor::AdjustmentControlStyle::Relative), styleCombo->currentData().toInt());

    styleCombo->setCurrentIndex(styleCombo->findData(static_cast<int>(editor::AdjustmentControlStyle::Classic)));
    buttons->button(QDialogButtonBox::Ok)->click();

    EXPECT_EQ(QDialog::Accepted, dialog.result());
    EXPECT_EQ(editor::AdjustmentControlStyle::Classic, EditorSettings(storage).loadAdjustmentControlStyle());
}

TEST(SettingsDialogTest, CreatesWorkerProfileThroughQtFreeClient)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings storage(QDir(directory.path()).filePath(QStringLiteral("settings.ini")), QSettings::IniFormat);
    QtWorkerProfileSettingsAdapter workerProfiles(storage);
    QtExportSettingsAdapter exportDefaults(storage);
    SettingsDialog dialog(storage, workerProfiles, exportDefaults);
    QPushButton* newButton = dialog.findChild<QPushButton*>(QStringLiteral("workerProfileNewButton"));
    QPushButton* saveButton = dialog.findChild<QPushButton*>(QStringLiteral("workerProfileSaveButton"));
    QLineEdit* nameEdit = dialog.findChild<QLineEdit*>(QStringLiteral("workerProfileNameEdit"));
    QLineEdit* hostEdit = dialog.findChild<QLineEdit*>(QStringLiteral("workerProfileHostEdit"));
    QSpinBox* portSpinBox = dialog.findChild<QSpinBox*>(QStringLiteral("workerProfilePortSpinBox"));
    ASSERT_NE(nullptr, newButton);
    ASSERT_NE(nullptr, saveButton);
    ASSERT_NE(nullptr, nameEdit);
    ASSERT_NE(nullptr, hostEdit);
    ASSERT_NE(nullptr, portSpinBox);

    newButton->click();
    nameEdit->setText(QStringLiteral("Jetson"));
    hostEdit->setText(QStringLiteral("192.168.1.90"));
    portSpinBox->setValue(48000);
    saveButton->click();

    const core::client::WorkerProfileListResult profiles = workerProfiles.listWorkerProfiles();
    ASSERT_TRUE(profiles.hasValue());
    ASSERT_EQ(2U, profiles.value().size());
    EXPECT_EQ("Jetson", profiles.value().back().displayName);
    EXPECT_EQ("192.168.1.90", profiles.value().back().host);
    EXPECT_EQ(48000, profiles.value().back().port);
}

}  // namespace
}  // namespace flexraw::ui::settings

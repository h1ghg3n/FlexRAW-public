#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "editor_settings.h"
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
    SettingsDialog dialog(storage);
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

}  // namespace
}  // namespace flexraw::ui::settings

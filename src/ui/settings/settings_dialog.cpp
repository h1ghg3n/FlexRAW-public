#include "settings_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>

#include "editor_settings.h"

namespace flexraw::ui::settings
{

// 목적: 현재 UI preference를 편집하는 최소 Settings window 초기화
// 입력: settings: application preference 저장소, parent: Qt 부모 widget
// 출력: View tab과 adjustment style selector를 포함한 dialog
SettingsDialog::SettingsDialog(QSettings& settings, QWidget* parent) : QDialog(parent), m_settings(settings)
{
    setWindowTitle(tr("Settings"));
    setModal(true);
    resize(460, 260);

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("settingsTabs"));
    auto* viewPage = new QWidget(tabs);
    auto* viewLayout = new QFormLayout(viewPage);
    m_adjustmentControlStyleComboBox = new QComboBox(viewPage);
    m_adjustmentControlStyleComboBox->setObjectName(QStringLiteral("adjustmentControlStyleComboBox"));
    m_adjustmentControlStyleComboBox->addItem(tr("Classic"), static_cast<int>(editor::AdjustmentControlStyle::Classic));
    m_adjustmentControlStyleComboBox->addItem(tr("Relative"),
                                              static_cast<int>(editor::AdjustmentControlStyle::Relative));
    viewLayout->addRow(tr("Adjustment control style"), m_adjustmentControlStyleComboBox);
    auto* description = new QLabel(
        tr("Classic shows a persistent value slider. Relative returns to a neutral center after each gesture."),
        viewPage);
    description->setWordWrap(true);
    viewLayout->addRow(QString{}, description);
    tabs->addTab(viewPage, tr("View"));
    layout->addWidget(tabs);

    const editor::AdjustmentControlStyle storedStyle = EditorSettings(m_settings).loadAdjustmentControlStyle();
    m_adjustmentControlStyleComboBox->setCurrentIndex(
        m_adjustmentControlStyleComboBox->findData(static_cast<int>(storedStyle)));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        EditorSettings(m_settings).saveAdjustmentControlStyle(adjustmentControlStyle());
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

// 목적: dialog에서 현재 선택된 adjustment presentation 반환
// 입력: 없음
// 출력: Classic 또는 Relative style
editor::AdjustmentControlStyle SettingsDialog::adjustmentControlStyle() const
{
    return static_cast<editor::AdjustmentControlStyle>(m_adjustmentControlStyleComboBox->currentData().toInt());
}

}  // namespace flexraw::ui::settings

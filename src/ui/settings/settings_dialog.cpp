#include "settings_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>

#include "editor_settings.h"
#include "export_defaults_page.h"
#include "general_settings_page.h"
#include "worker_profiles_page.h"

namespace flexraw::ui::settings
{

// 목적: 현재 UI preference를 편집하는 최소 Settings window 초기화
// 입력: settings: application preference 저장소, workerProfileClient: Worker CRUD,
//       exportDefaultsClient: application Export 기본값 계약, workerHealthClient/eventSource: optional health,
//       catalogStartupSettingsClient: optional General settings capability, parent: Qt 부모 widget
// 출력: available General capability, View, Workers와 Export tab을 포함한 dialog
SettingsDialog::SettingsDialog(QSettings& settings,
                               core::client::IWorkerProfileClient& workerProfileClient,
                               core::client::IExportDefaultsClient& exportDefaultsClient,
                               core::client::IWorkerHealthClient* const workerHealthClient,
                               core::client::IWorkerHealthEventSource* const workerHealthEventSource,
                               core::client::ICatalogStartupSettingsClient* const catalogStartupSettingsClient,
                               QWidget* parent)
    : QDialog(parent), m_settings(settings)
{
    setWindowTitle(tr("Settings"));
    setModal(true);
    resize(560, 460);

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("settingsTabs"));
    if (catalogStartupSettingsClient != nullptr)
    {
        m_generalSettingsPage = new GeneralSettingsPage(*catalogStartupSettingsClient, tabs);
        tabs->addTab(m_generalSettingsPage, tr("General"));
    }
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
    tabs->addTab(new WorkerProfilesPage(workerProfileClient, workerHealthClient, workerHealthEventSource, tabs),
                 tr("Workers"));
    m_exportDefaultsPage = new ExportDefaultsPage(exportDefaultsClient, workerProfileClient, tabs);
    tabs->addTab(m_exportDefaultsPage, tr("Export"));
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](const int index) {
        if (tabs->widget(index) == m_exportDefaultsPage)
        {
            m_exportDefaultsPage->refreshWorkerProfiles();
        }
    });
    layout->addWidget(tabs);

    const editor::AdjustmentControlStyle storedStyle = EditorSettings(m_settings).loadAdjustmentControlStyle();
    m_adjustmentControlStyleComboBox->setCurrentIndex(
        m_adjustmentControlStyleComboBox->findData(static_cast<int>(storedStyle)));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (m_generalSettingsPage != nullptr && !m_generalSettingsPage->saveSettings())
        {
            return;
        }
        if (!m_exportDefaultsPage->saveDefaults())
        {
            return;
        }
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

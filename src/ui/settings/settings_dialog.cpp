#include "settings_dialog.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>

#include "editor_settings.h"
#include "export_defaults_page.h"
#include "general_settings_page.h"
#include "settings_page_header.h"
#include "worker_profiles_page.h"

namespace flexraw::ui::settings
{
namespace
{

constexpr auto UiSettingsGroup = "ui";
constexpr auto SettingsDialogGeometryKey = "settingsDialogGeometry";
constexpr auto SettingsDialogPageKey = "settingsDialogPage";

struct ViewSettingsPage
{
    QWidget* page{nullptr};
    QComboBox* adjustmentControlStyle{nullptr};
};

// 목적: frontend-only adjustment presentation을 독립 View page로 구성
// 입력: parent: Settings tab widget
// 출력: page widget과 SettingsDialog가 저장할 control style selector
[[nodiscard]] ViewSettingsPage createViewSettingsPage(QWidget* parent)
{
    const auto text = [](const char* source) { return QCoreApplication::translate("SettingsDialog", source); };
    auto* page = new QWidget(parent);
    page->setObjectName(QStringLiteral("viewSettingsPage"));
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new SettingsPageHeader(
        text("View"), text("Choose how editing controls are presented without changing photo adjustments."), page));
    auto* adjustmentGroup = new QGroupBox(text("Adjustment controls"), page);
    auto* adjustmentLayout = new QFormLayout(adjustmentGroup);
    auto* controlStyle = new QComboBox(page);
    controlStyle->setObjectName(QStringLiteral("adjustmentControlStyleComboBox"));
    controlStyle->addItem(text("Classic"), static_cast<int>(editor::AdjustmentControlStyle::Classic));
    controlStyle->addItem(text("Relative"), static_cast<int>(editor::AdjustmentControlStyle::Relative));
    adjustmentLayout->addRow(text("Control style"), controlStyle);
    auto* description = new QLabel(
        text("Classic shows a persistent value slider. Relative returns to a neutral center after each gesture."),
        adjustmentGroup);
    description->setWordWrap(true);
    adjustmentLayout->addRow(QString{}, description);
    layout->addWidget(adjustmentGroup);
    layout->addStretch(1);
    return {page, controlStyle};
}

// 목적: frontend-only Settings window geometry와 마지막 page 복원
// 입력: settings: application preference 저장소, dialog/tabs: 복원 대상 widget
// 출력: 저장값이 유효한 범위에서 window와 current page 갱신
void restoreDialogState(QSettings& settings, QDialog& dialog, QTabWidget& tabs)
{
    settings.beginGroup(QString::fromLatin1(UiSettingsGroup));
    const QByteArray geometry = settings.value(QString::fromLatin1(SettingsDialogGeometryKey)).toByteArray();
    const QString pageName = settings.value(QString::fromLatin1(SettingsDialogPageKey)).toString();
    settings.endGroup();

    if (!geometry.isEmpty())
    {
        (void)dialog.restoreGeometry(geometry);
    }
    for (int index = 0; index < tabs.count(); ++index)
    {
        if (!pageName.isEmpty() && tabs.widget(index)->objectName() == pageName)
        {
            tabs.setCurrentIndex(index);
            break;
        }
    }
}

// 목적: frontend-only Settings window geometry와 current page 저장
// 입력: settings: application preference 저장소, dialog/tabs: 현재 presentation state
// 출력: 다음 Settings open에서 복원할 UI preference 갱신
void saveDialogState(QSettings& settings, const QDialog& dialog, const QTabWidget& tabs)
{
    settings.beginGroup(QString::fromLatin1(UiSettingsGroup));
    settings.setValue(QString::fromLatin1(SettingsDialogGeometryKey), dialog.saveGeometry());
    if (tabs.currentWidget() != nullptr)
    {
        settings.setValue(QString::fromLatin1(SettingsDialogPageKey), tabs.currentWidget()->objectName());
    }
    settings.endGroup();
    settings.sync();
}

}  // namespace

// 목적: application preference와 product default를 편집하는 Settings window 초기화
// 입력: settings: application preference 저장소, workerProfileClient: Worker CRUD,
//       exportDefaultsClient: application Export 기본값 계약, workerHealthClient/eventSource: optional health,
//       catalogStartupSettingsClient: optional General settings capability, parent: Qt 부모 widget
// 출력: 상태 복원, 저장 의미 안내와 General/View/Workers/Export page를 포함한 dialog
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
    resize(720, 560);
    setMinimumSize(640, 480);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 16);
    layout->setSpacing(12);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("settingsTabs"));
    tabs->setAccessibleName(tr("Settings categories"));
    if (catalogStartupSettingsClient != nullptr)
    {
        m_generalSettingsPage = new GeneralSettingsPage(*catalogStartupSettingsClient, tabs);
        tabs->addTab(m_generalSettingsPage, tr("General"));
    }
    const ViewSettingsPage view = createViewSettingsPage(tabs);
    m_adjustmentControlStyleComboBox = view.adjustmentControlStyle;
    tabs->addTab(view.page, tr("View"));
    auto* workerProfilesPage =
        new WorkerProfilesPage(workerProfileClient, workerHealthClient, workerHealthEventSource, tabs);
    workerProfilesPage->setObjectName(QStringLiteral("workerProfilesPage"));
    tabs->addTab(workerProfilesPage, tr("Workers"));
    m_exportDefaultsPage = new ExportDefaultsPage(exportDefaultsClient, workerProfileClient, tabs);
    tabs->addTab(m_exportDefaultsPage, tr("Export"));
    connect(tabs, &QTabWidget::currentChanged, this, [this, tabs](const int index) {
        if (tabs->widget(index) == m_exportDefaultsPage)
        {
            m_exportDefaultsPage->refreshWorkerProfiles();
        }
    });
    layout->addWidget(tabs);
    restoreDialogState(m_settings, *this, *tabs);

    const editor::AdjustmentControlStyle storedStyle = EditorSettings(m_settings).loadAdjustmentControlStyle();
    m_adjustmentControlStyleComboBox->setCurrentIndex(
        m_adjustmentControlStyleComboBox->findData(static_cast<int>(storedStyle)));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("settingsDialogButtons"));
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
    connect(this, &QDialog::finished, this, [this, tabs] { saveDialogState(m_settings, *this, *tabs); });
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

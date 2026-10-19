#include "general_settings_page.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QString>
#include <QVBoxLayout>
#include <QtGlobal>

#include "settings_page_header.h"

namespace flexraw::ui::settings
{

// 목적: Catalog startup product setting과 현재 application 기본 정보를 표시하는 General page 구성
// 입력: startupSettingsClient: application startup settings 계약, parent: Qt 부모 widget
// 출력: persisted startup 정책과 Managed Catalog 위치를 표시하는 Settings page
GeneralSettingsPage::GeneralSettingsPage(core::client::ICatalogStartupSettingsClient& startupSettingsClient,
                                         QWidget* parent)
    : QWidget(parent),
      m_startupSettingsClient(&startupSettingsClient),
      m_startupBehaviorComboBox(new QComboBox(this)),
      m_managedCatalogPathEdit(new QLineEdit(this)),
      m_statusLabel(new QLabel(this))
{
    setObjectName(QStringLiteral("generalSettingsPage"));

    m_startupBehaviorComboBox->setObjectName(QStringLiteral("catalogStartupBehaviorComboBox"));
    m_startupBehaviorComboBox->addItem(tr("Reopen the last active Catalog"),
                                       static_cast<int>(core::client::CatalogStartupBehavior::ReopenLastActive));
    m_startupBehaviorComboBox->addItem(tr("Open the managed Catalog"),
                                       static_cast<int>(core::client::CatalogStartupBehavior::OpenManagedCatalog));

    m_managedCatalogPathEdit->setObjectName(QStringLiteral("managedCatalogPathEdit"));
    m_managedCatalogPathEdit->setReadOnly(true);
    m_managedCatalogPathEdit->setToolTip(tr("Resolved per-user Managed Catalog location"));

    auto* catalogGroup = new QGroupBox(tr("Catalog startup"), this);
    auto* catalogLayout = new QFormLayout(catalogGroup);
    catalogLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    catalogLayout->addRow(tr("On startup"), m_startupBehaviorComboBox);
    catalogLayout->addRow(tr("Managed Catalog"), m_managedCatalogPathEdit);
    auto* restartNotice = new QLabel(tr("Changes take effect the next time Flexraw starts."), catalogGroup);
    restartNotice->setWordWrap(true);
    catalogLayout->addRow(QString{}, restartNotice);

    auto* languageGroup = new QGroupBox(tr("Language"), this);
    auto* languageLayout = new QFormLayout(languageGroup);
    auto* languageValue = new QLabel(tr("English"), languageGroup);
    languageValue->setObjectName(QStringLiteral("currentInterfaceLanguageLabel"));
    languageLayout->addRow(tr("Interface language"), languageValue);
    auto* languageNotice =
        new QLabel(tr("Additional interface languages are not installed in this build."), languageGroup);
    languageNotice->setWordWrap(true);
    languageLayout->addRow(QString{}, languageNotice);

    auto* applicationGroup = new QGroupBox(tr("Application information"), this);
    auto* applicationLayout = new QFormLayout(applicationGroup);
    const QString applicationVersion = QCoreApplication::applicationVersion().trimmed();
    auto* versionValue =
        new QLabel(applicationVersion.isEmpty() ? tr("Development build") : applicationVersion, applicationGroup);
    versionValue->setObjectName(QStringLiteral("applicationVersionLabel"));
    versionValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* qtVersionValue = new QLabel(QString::fromLatin1(qVersion()), applicationGroup);
    qtVersionValue->setObjectName(QStringLiteral("qtRuntimeVersionLabel"));
    qtVersionValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
    applicationLayout->addRow(tr("Flexraw version"), versionValue);
    applicationLayout->addRow(tr("Qt runtime"), qtVersionValue);

    m_statusLabel->setObjectName(QStringLiteral("generalSettingsStatusLabel"));
    m_statusLabel->setWordWrap(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new SettingsPageHeader(
        tr("General"), tr("Configure startup behavior and review information about this installation."), this));
    layout->addWidget(catalogGroup);
    layout->addWidget(languageGroup);
    layout->addWidget(applicationGroup);
    layout->addWidget(m_statusLabel);
    layout->addStretch(1);

    loadSettings();
}

// 목적: 현재 선택한 Catalog startup 정책을 다음 application 실행용으로 저장
// 입력: 없음
// 출력: 저장 성공 시 true, load 또는 persistence 실패 시 false
bool GeneralSettingsPage::saveSettings()
{
    if (!m_settingsLoaded)
    {
        m_statusLabel->setText(tr("General settings are unavailable."));
        return false;
    }

    const auto behavior =
        static_cast<core::client::CatalogStartupBehavior>(m_startupBehaviorComboBox->currentData().toInt());
    const core::client::CatalogStartupSettingsResult result =
        m_startupSettingsClient->saveCatalogStartupBehavior(behavior);
    if (result.hasError())
    {
        m_statusLabel->setText(tr("General settings could not be saved."));
        return false;
    }

    m_statusLabel->setText(tr("General settings saved."));
    return true;
}

// 목적: 저장된 Catalog startup settings snapshot을 controls에 복원
// 입력: 없음
// 출력: 성공 시 저장 가능한 controls와 Managed Catalog 위치 갱신
void GeneralSettingsPage::loadSettings()
{
    const core::client::CatalogStartupSettingsResult result = m_startupSettingsClient->catalogStartupSettings();
    if (result.hasError())
    {
        m_startupBehaviorComboBox->setEnabled(false);
        m_managedCatalogPathEdit->clear();
        m_statusLabel->setText(tr("General settings could not be loaded."));
        return;
    }

    const core::client::CatalogStartupSettingsSnapshot& settings = result.value();
    m_startupBehaviorComboBox->setCurrentIndex(
        m_startupBehaviorComboBox->findData(static_cast<int>(settings.behavior)));
    m_managedCatalogPathEdit->setText(QString::fromUtf8(settings.managedCatalogPath.c_str()));
    m_settingsLoaded = true;
}

}  // namespace flexraw::ui::settings

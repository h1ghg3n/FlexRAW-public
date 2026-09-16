#include "worker_profiles_page.h"

#include <algorithm>

#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "worker_health_panel.h"

namespace flexraw::ui::settings
{

// 목적: application-level Worker profile CRUD를 편집하는 Settings page 구성
// 입력: workerProfileClient: Qt-free profile contract, healthClient/eventSource: optional health capability,
//       parent: Qt 부모 widget
// 출력: profile 목록과 명시적 New/Save/Delete control을 가진 page
WorkerProfilesPage::WorkerProfilesPage(core::client::IWorkerProfileClient& workerProfileClient,
                                       core::client::IWorkerHealthClient* const healthClient,
                                       core::client::IWorkerHealthEventSource* const healthEventSource,
                                       QWidget* parent)
    : QWidget(parent),
      m_workerProfileClient(&workerProfileClient),
      m_profileList(new QListWidget(this)),
      m_displayNameEdit(new QLineEdit(this)),
      m_hostEdit(new QLineEdit(this)),
      m_portSpinBox(new QSpinBox(this)),
      m_enabledCheckBox(new QCheckBox(tr("Enabled"), this)),
      m_sourceStorageIdEdit(new QLineEdit(this)),
      m_outputStorageIdEdit(new QLineEdit(this)),
      m_newButton(new QPushButton(tr("New"), this)),
      m_saveButton(new QPushButton(tr("Save"), this)),
      m_deleteButton(new QPushButton(tr("Delete"), this)),
      m_statusLabel(new QLabel(this)),
      m_healthPanel(new WorkerHealthPanel(healthClient, healthEventSource, this))
{
    m_profileList->setObjectName(QStringLiteral("workerProfileList"));
    m_profileList->setMinimumWidth(150);
    m_displayNameEdit->setObjectName(QStringLiteral("workerProfileNameEdit"));
    m_hostEdit->setObjectName(QStringLiteral("workerProfileHostEdit"));
    m_portSpinBox->setObjectName(QStringLiteral("workerProfilePortSpinBox"));
    m_portSpinBox->setRange(1, 65535);
    m_enabledCheckBox->setObjectName(QStringLiteral("workerProfileEnabledCheckBox"));
    m_sourceStorageIdEdit->setObjectName(QStringLiteral("workerProfileSourceStorageIdEdit"));
    m_outputStorageIdEdit->setObjectName(QStringLiteral("workerProfileOutputStorageIdEdit"));
    m_newButton->setObjectName(QStringLiteral("workerProfileNewButton"));
    m_saveButton->setObjectName(QStringLiteral("workerProfileSaveButton"));
    m_deleteButton->setObjectName(QStringLiteral("workerProfileDeleteButton"));
    m_statusLabel->setObjectName(QStringLiteral("workerProfileStatusLabel"));
    m_statusLabel->setWordWrap(true);

    auto* editorLayout = new QFormLayout();
    editorLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    editorLayout->addRow(tr("Name"), m_displayNameEdit);
    editorLayout->addRow(tr("Host"), m_hostEdit);
    editorLayout->addRow(tr("Port"), m_portSpinBox);
    editorLayout->addRow(QString{}, m_enabledCheckBox);
    editorLayout->addRow(tr("Expected source storage ID"), m_sourceStorageIdEdit);
    editorLayout->addRow(tr("Expected output storage ID"), m_outputStorageIdEdit);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_newButton);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(m_deleteButton);
    buttonLayout->addWidget(m_saveButton);
    auto* rightLayout = new QVBoxLayout();
    rightLayout->addLayout(editorLayout);
    rightLayout->addWidget(m_statusLabel);
    rightLayout->addWidget(m_healthPanel);
    rightLayout->addStretch(1);
    rightLayout->addLayout(buttonLayout);
    auto* layout = new QHBoxLayout(this);
    layout->addWidget(m_profileList);
    layout->addLayout(rightLayout, 1);

    connect(m_profileList, &QListWidget::currentRowChanged, this, &WorkerProfilesPage::selectProfile);
    connect(m_newButton, &QPushButton::clicked, this, &WorkerProfilesPage::beginNewProfile);
    connect(m_saveButton, &QPushButton::clicked, this, &WorkerProfilesPage::saveProfile);
    connect(m_deleteButton, &QPushButton::clicked, this, &WorkerProfilesPage::deleteProfile);
    refreshProfiles();
}

// 목적: 저장소 snapshot으로 profile 목록을 다시 구성하고 optional identity 선택
// 입력: selectedId: refresh 뒤 유지할 stable identity
// 출력: 목록·editor·상태 표시 갱신
void WorkerProfilesPage::refreshProfiles(const std::optional<core::client::WorkerProfileId>& selectedId)
{
    const core::client::WorkerProfileListResult result = m_workerProfileClient->listWorkerProfiles();
    m_profileList->clear();
    m_profiles.clear();
    if (result.hasError())
    {
        m_statusLabel->setText(tr("Worker profiles could not be loaded."));
        beginNewProfile();
        m_saveButton->setEnabled(false);
        return;
    }

    m_profiles = result.value();
    for (const core::client::WorkerProfileSnapshot& profile : m_profiles)
    {
        const QString name = QString::fromUtf8(profile.displayName.c_str());
        auto* item = new QListWidgetItem(profile.enabled ? name : tr("%1 (Disabled)").arg(name), m_profileList);
        item->setData(Qt::UserRole, QString::fromStdString(profile.id.value));
    }
    m_saveButton->setEnabled(true);
    m_statusLabel->clear();

    int selectedRow = -1;
    if (selectedId.has_value())
    {
        for (int row = 0; row < m_profileList->count(); ++row)
        {
            if (m_profileList->item(row)->data(Qt::UserRole).toString() == QString::fromStdString(selectedId->value))
            {
                selectedRow = row;
                break;
            }
        }
    }
    if (selectedRow < 0 && !m_profiles.empty())
    {
        selectedRow = 0;
    }
    if (selectedRow >= 0)
    {
        m_profileList->setCurrentRow(selectedRow);
    }
    else
    {
        beginNewProfile();
    }
}

// 목적: 현재 list selection의 profile 값을 editor field에 표시
// 입력: row: 선택된 list row 또는 -1
// 출력: selection identity와 field/button 상태 갱신
void WorkerProfilesPage::selectProfile(const int row)
{
    if (row < 0 || row >= static_cast<int>(m_profiles.size()))
    {
        m_selectedProfileId.reset();
        m_deleteButton->setEnabled(false);
        m_healthPanel->setProfile(std::nullopt);
        return;
    }

    const core::client::WorkerProfileSnapshot& profile = m_profiles.at(static_cast<std::size_t>(row));
    m_selectedProfileId = profile.id;
    m_displayNameEdit->setText(QString::fromUtf8(profile.displayName.c_str()));
    m_hostEdit->setText(QString::fromUtf8(profile.host.c_str()));
    m_portSpinBox->setValue(profile.port);
    m_enabledCheckBox->setChecked(profile.enabled);
    m_sourceStorageIdEdit->setText(QString::fromStdString(profile.expectedSourceStorageId));
    m_outputStorageIdEdit->setText(QString::fromStdString(profile.expectedOutputStorageId));
    m_deleteButton->setEnabled(true);
    m_statusLabel->clear();
    m_healthPanel->setProfile(profile);
}

// 목적: 새 profile 입력을 위한 기본 editor 상태 시작
// 입력: 없음
// 출력: selection 해제와 loopback endpoint 기본값 표시
void WorkerProfilesPage::beginNewProfile()
{
    m_profileList->clearSelection();
    m_profileList->setCurrentRow(-1);
    m_selectedProfileId.reset();
    m_displayNameEdit->clear();
    m_hostEdit->setText(QStringLiteral("127.0.0.1"));
    m_portSpinBox->setValue(47331);
    m_enabledCheckBox->setChecked(true);
    m_sourceStorageIdEdit->clear();
    m_outputStorageIdEdit->clear();
    m_deleteButton->setEnabled(false);
    m_healthPanel->setProfile(std::nullopt);
    m_statusLabel->setText(tr("Enter a name and endpoint, then save the profile."));
    m_displayNameEdit->setFocus();
}

// 목적: editor field를 create 또는 update command로 명시적 저장
// 입력: 없음
// 출력: 성공 시 stable identity 재선택, 실패 시 일반 사용자 안내
void WorkerProfilesPage::saveProfile()
{
    core::client::WorkerProfileResult result =
        m_selectedProfileId.has_value()
            ? m_workerProfileClient->updateWorkerProfile(core::client::UpdateWorkerProfileCommand{
                  *m_selectedProfileId,
                  m_displayNameEdit->text().toUtf8().toStdString(),
                  m_hostEdit->text().toUtf8().toStdString(),
                  static_cast<std::uint16_t>(m_portSpinBox->value()),
                  m_enabledCheckBox->isChecked(),
                  m_sourceStorageIdEdit->text().toStdString(),
                  m_outputStorageIdEdit->text().toStdString(),
              })
            : m_workerProfileClient->createWorkerProfile(core::client::CreateWorkerProfileCommand{
                  m_displayNameEdit->text().toUtf8().toStdString(),
                  m_hostEdit->text().toUtf8().toStdString(),
                  static_cast<std::uint16_t>(m_portSpinBox->value()),
                  m_enabledCheckBox->isChecked(),
                  m_sourceStorageIdEdit->text().toStdString(),
                  m_outputStorageIdEdit->text().toStdString(),
              });
    if (result.hasError())
    {
        m_statusLabel->setText(result.error().code == core::client::ClientErrorCode::Conflict
                                   ? tr("A Worker profile with that name already exists.")
                                   : tr("Check the Worker profile values and try again."));
        return;
    }
    refreshProfiles(result.value().id);
    m_statusLabel->setText(tr("Worker profile saved."));
}

// 목적: 선택된 profile을 사용자 확인 뒤 application settings에서 제거
// 입력: 없음
// 출력: 성공 시 목록 refresh, 실패 시 일반 사용자 안내
void WorkerProfilesPage::deleteProfile()
{
    if (!m_selectedProfileId.has_value())
    {
        return;
    }
    if (QMessageBox::question(this,
                              tr("Delete Worker Profile"),
                              tr("Delete the selected Worker profile?"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
    {
        return;
    }
    const core::client::WorkerProfileRemoveResult removed =
        m_workerProfileClient->removeWorkerProfile(*m_selectedProfileId);
    if (removed.hasError())
    {
        m_statusLabel->setText(tr("Worker profile could not be deleted."));
        return;
    }
    refreshProfiles();
    m_statusLabel->setText(tr("Worker profile deleted."));
}

}  // namespace flexraw::ui::settings

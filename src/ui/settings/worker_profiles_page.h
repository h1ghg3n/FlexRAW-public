#pragma once

#include <optional>
#include <vector>

#include <QWidget>

#include "worker_health_client.h"
#include "worker_profile_client.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;

namespace flexraw::ui::settings
{

class WorkerHealthPanel;

class WorkerProfilesPage final : public QWidget
{
public:
    // 목적: application-level Worker profile CRUD를 편집하는 Settings page 구성
    // 입력: workerProfileClient: Qt-free profile contract, healthClient/eventSource: optional health capability,
    //       parent: Qt 부모 widget
    // 출력: profile 목록과 명시적 New/Save/Delete control을 가진 page
    explicit WorkerProfilesPage(core::client::IWorkerProfileClient& workerProfileClient,
                                core::client::IWorkerHealthClient* healthClient = nullptr,
                                core::client::IWorkerHealthEventSource* healthEventSource = nullptr,
                                QWidget* parent = nullptr);

private:
    // 목적: 저장소 snapshot으로 profile 목록을 다시 구성하고 optional identity 선택
    // 입력: selectedId: refresh 뒤 유지할 stable identity
    // 출력: 목록·editor·상태 표시 갱신
    void refreshProfiles(const std::optional<core::client::WorkerProfileId>& selectedId = std::nullopt);

    // 목적: 현재 list selection의 profile 값을 editor field에 표시
    // 입력: row: 선택된 list row 또는 -1
    // 출력: selection identity와 field/button 상태 갱신
    void selectProfile(int row);

    // 목적: 새 profile 입력을 위한 기본 editor 상태 시작
    // 입력: 없음
    // 출력: selection 해제와 loopback endpoint 기본값 표시
    void beginNewProfile();

    // 목적: editor field를 create 또는 update command로 명시적 저장
    // 입력: 없음
    // 출력: 성공 시 stable identity 재선택, 실패 시 일반 사용자 안내
    void saveProfile();

    // 목적: 선택된 profile을 사용자 확인 뒤 application settings에서 제거
    // 입력: 없음
    // 출력: 성공 시 목록 refresh, 실패 시 일반 사용자 안내
    void deleteProfile();

    core::client::IWorkerProfileClient* m_workerProfileClient{nullptr};
    std::vector<core::client::WorkerProfileSnapshot> m_profiles;
    std::optional<core::client::WorkerProfileId> m_selectedProfileId;
    QListWidget* m_profileList{nullptr};
    QLineEdit* m_displayNameEdit{nullptr};
    QLineEdit* m_hostEdit{nullptr};
    QSpinBox* m_portSpinBox{nullptr};
    QCheckBox* m_enabledCheckBox{nullptr};
    QLineEdit* m_sourceStorageIdEdit{nullptr};
    QLineEdit* m_outputStorageIdEdit{nullptr};
    QPushButton* m_newButton{nullptr};
    QPushButton* m_saveButton{nullptr};
    QPushButton* m_deleteButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    WorkerHealthPanel* m_healthPanel{nullptr};
};

}  // namespace flexraw::ui::settings

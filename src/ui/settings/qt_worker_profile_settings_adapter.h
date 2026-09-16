#pragma once

#include "worker_profile_client.h"

class QSettings;

namespace flexraw::ui::settings
{

class QtWorkerProfileSettingsAdapter final : public core::client::IWorkerProfileClient
{
public:
    // 목적: application settings를 Worker profile contract 저장소로 연결
    // 입력: settings: adapter보다 오래 유지되는 application QSettings
    // 출력: lazy schema migration이 가능한 profile client
    explicit QtWorkerProfileSettingsAdapter(QSettings& settings);

    // 목적: 저장 순서가 보존된 현재 Worker profile snapshot 목록 조회
    // 입력: 없음
    // 출력: profile 목록 또는 schema·저장소 오류
    [[nodiscard]] core::client::WorkerProfileListResult listWorkerProfiles() override;

    // 목적: 검증된 Worker profile을 새 UUID identity로 저장
    // 입력: command: 이름, endpoint, 활성화 상태와 expected storage binding
    // 출력: 생성된 normalized profile 또는 validation·저장소 오류
    [[nodiscard]] core::client::WorkerProfileResult createWorkerProfile(
        const core::client::CreateWorkerProfileCommand& command) override;

    // 목적: stable identity를 유지하며 검증된 Worker profile 전체 갱신
    // 입력: command: 대상 identity와 교체할 profile 속성
    // 출력: 갱신된 normalized profile 또는 validation·not-found·저장소 오류
    [[nodiscard]] core::client::WorkerProfileResult updateWorkerProfile(
        const core::client::UpdateWorkerProfileCommand& command) override;

    // 목적: Worker profile과 저장 순서 entry 제거
    // 입력: id: 제거할 stable profile identity
    // 출력: 제거된 identity 또는 validation·not-found·저장소 오류
    [[nodiscard]] core::client::WorkerProfileRemoveResult removeWorkerProfile(
        const core::client::WorkerProfileId& id) override;

    // 목적: 다른 profile 속성을 유지하며 enabled 정책 갱신
    // 입력: command: 대상 identity와 새 enabled 상태
    // 출력: 갱신된 profile 또는 validation·not-found·저장소 오류
    [[nodiscard]] core::client::WorkerProfileResult setWorkerProfileEnabled(
        const core::client::SetWorkerProfileEnabledCommand& command) override;

private:
    QSettings& m_settings;
};

}  // namespace flexraw::ui::settings

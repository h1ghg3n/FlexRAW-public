#pragma once

#include <optional>

#include <QGroupBox>

#include "worker_health_client.h"
#include "worker_profile_client.h"

class QLabel;
class QPushButton;

namespace flexraw::ui::settings
{

class WorkerHealthPanel final : public QGroupBox
{
public:
    // 목적: 선택된 Worker profile의 manual one-shot health control과 snapshot 표시 구성
    // 입력: healthClient/eventSource: optional Product health capability, parent: Qt 부모 widget
    // 출력: Test/Refresh와 reachability/load/timing presentation을 가진 panel
    WorkerHealthPanel(core::client::IWorkerHealthClient* healthClient,
                      core::client::IWorkerHealthEventSource* eventSource,
                      QWidget* parent = nullptr);

    // 목적: UI state보다 먼저 health callback 입구를 명시적으로 차단
    // 입력: 없음
    // 출력: queued/future callback이 이 panel에 도달하지 않는 상태
    ~WorkerHealthPanel() override;

    // 목적: health control이 관찰할 현재 Worker profile 교체
    // 입력: profile: 저장된 profile 또는 new-profile 편집 상태의 nullopt
    // 출력: endpoint에 맞는 최근 snapshot 또는 Not checked 표시
    void setProfile(std::optional<core::client::WorkerProfileSnapshot> profile);

private:
    // 목적: 선택 profile에 one-shot probe를 제출하고 checking 상태 표시
    // 입력: 없음
    // 출력: accepted면 button 비활성화, immediate 오류면 일반 사용자 안내
    void requestProbe();

    // 목적: serialized Product health event를 현재 선택 profile presentation에 반영
    // 입력: event: active probe와 optional terminal snapshot
    // 출력: checking 또는 최신 health 표시
    void applyEvent(const core::client::WorkerHealthEvent& event);

    // 목적: Health Client cache에서 현재 endpoint의 최근 snapshot 조회
    // 입력: 없음
    // 출력: snapshot 또는 generic unavailable 표시
    void refreshSnapshot();

    // 목적: structured health dimensions를 localized Settings text로 표시
    // 입력: snapshot: 현재 profile endpoint에 대응하는 observation
    // 출력: summary/load/timing label과 Test button 상태 갱신
    void renderSnapshot(const core::client::WorkerHealthSnapshot& snapshot);

    // 목적: 선택 profile 또는 health capability가 없는 상태 표시
    // 입력: 없음
    // 출력: detail 초기화와 Test button 비활성화
    void renderUnavailable();

    core::client::IWorkerHealthClient* m_healthClient{nullptr};
    core::client::WorkerHealthSubscriptionHandle m_subscription;
    std::optional<core::client::WorkerProfileSnapshot> m_profile;
    std::optional<core::client::WorkerHealthProbeReceipt> m_activeProbe;
    QPushButton* m_probeButton{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QLabel* m_loadLabel{nullptr};
    QLabel* m_timingLabel{nullptr};
};

}  // namespace flexraw::ui::settings

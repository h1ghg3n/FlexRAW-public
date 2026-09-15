#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "worker_health_client.h"

namespace flexraw::core::orchestration
{
class WorkerHealthOrchestrator;
}

namespace flexraw::ui::settings
{

class QtWorkerHealthEventAdapter final : public QObject, public core::client::IWorkerHealthEventSource
{
public:
    // 목적: Worker health operation state를 현재 Qt thread의 serialized Product event로 변환
    // 입력: orchestrator: authoritative probe/snapshot owner, parent: Qt lifetime owner
    // 출력: Qt delivery context에 bound된 event adapter
    explicit QtWorkerHealthEventAdapter(core::orchestration::WorkerHealthOrchestrator& orchestrator,
                                        QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtWorkerHealthEventAdapter() override;

    // 목적: initial active probe와 이후 accepted/terminal health event 구독
    // 입력: callback: immutable Worker health event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::WorkerHealthSubscriptionResult subscribeToWorkerHealth(
        core::client::WorkerHealthCallback callback) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    // 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
    // 입력: 없음
    // 출력: 다음 Worker health event sequence
    [[nodiscard]] core::client::WorkerHealthEventSequence nextEventSequence() noexcept;

    // 목적: 현재 active probe와 optional completion을 모든 active subscription에 fan-out
    // 입력: completion: accepted probe의 optional terminal snapshot
    // 출력: subscription별 Qt queued callback 등록
    void publishEvent(std::optional<core::client::WorkerHealthProbeCompletion> completion);

    // 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
    // 입력: state: subscription lifetime, event: 전달할 Worker health event
    // 출력: 전달 불필요 또는 queue 성공이면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::WorkerHealthEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 Worker health event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state, const core::client::WorkerHealthEvent& event) noexcept;

    core::orchestration::WorkerHealthOrchestrator* m_orchestrator{nullptr};
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::uint64_t m_nextEventSequence{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::ui::settings

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <QObject>

#include "editor_event_client.h"

namespace flexraw::core::orchestration
{
class EditorOrchestrator;
}

namespace flexraw::ui::facade
{

class QtEditorEventAdapter final : public QObject, public core::client::IEditorStateEventSource
{
public:
    // 목적: Editor snapshot invalidation을 현재 Qt thread의 serialized client event로 변환
    // 입력: editorOrchestrator: authoritative Editor session, parent: Qt lifetime owner
    // 출력: Qt delivery context에 bound된 event adapter
    explicit QtEditorEventAdapter(core::orchestration::EditorOrchestrator& editorOrchestrator,
                                  QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtEditorEventAdapter() override;

    // 목적: 현재 Qt delivery context에서 initial snapshot과 이후 Editor state event 구독
    // 입력: callback: immutable event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::EditorStateSubscriptionResult subscribeToEditorState(
        core::client::EditorStateCallback callback) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    // 목적: adapter lifetime에서 0을 사용하지 않는 단조 event sequence 발급
    // 입력: 없음
    // 출력: 다음 Editor event sequence
    [[nodiscard]] core::client::EditorEventSequence nextEventSequence() noexcept;

    // 목적: 현재 Orchestrator snapshot을 capture해 active subscription에 비동기 fan-out
    // 입력: 없음
    // 출력: Adjustment intermediate coalescing 또는 ordered queue 등록
    void publishCurrentSnapshot();

    // 목적: 한 subscription에 event를 Qt queued callback으로 등록
    // 입력: state: subscription lifetime, event: immutable Editor state event
    // 출력: 전달이 불필요하거나 queue 등록에 성공하면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::EditorStateEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 snapshot event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state, const core::client::EditorStateEvent& event) noexcept;

    // 목적: 같은 Adjustment burst에서 마지막으로 coalescing된 intermediate event 전달
    // 입력: state: pending event와 callback을 소유한 subscription
    // 출력: 없음
    static void deliverPendingAdjustment(const SubscriptionStatePtr& state) noexcept;

    core::orchestration::EditorOrchestrator* m_editorOrchestrator{nullptr};
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::uint64_t m_nextEventSequence{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::ui::facade

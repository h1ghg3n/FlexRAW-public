#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "activity_client.h"

namespace flexraw::ui::facade
{
class CatalogEditorFacade;
}

namespace flexraw::ui::mainwindow
{

class FolderScanController;

class QtActivityAdapter final : public QObject, public core::client::IActivityClient
{
public:
    // 목적: 기존 Qt owner lifecycle을 하나의 Qt-free Activity client로 집계
    // 입력: catalogEditorFacade: Preview/Source adapter, folderScanController: Folder scan owner, parent: Qt parent
    // 출력: 현재 Qt thread에 bound된 Activity adapter
    QtActivityAdapter(facade::CatalogEditorFacade& catalogEditorFacade,
                      FolderScanController& folderScanController,
                      QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving Activity subscription을 inactive로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtActivityAdapter() override;

    // 목적: initial active 목록과 이후 lifecycle event를 Qt delivery context에서 구독
    // 입력: callback: immutable Activity event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::ActivitySubscriptionResult subscribeToActivities(
        core::client::ActivityCallback callback) override;

    // 목적: cancellable Activity identity를 실제 Preview 또는 Source owner에 전달
    // 입력: activityId: kind와 owner request identity
    // 출력: owner가 cancellation을 수락한 identity 또는 stale·unsupported 오류
    [[nodiscard]] core::client::ActivityCancelResult cancelActivity(core::client::ActivityId activityId) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    // 목적: 0을 사용하지 않는 adapter-local event ordering sequence 발급
    // 입력: 없음
    // 출력: 다음 Activity event sequence
    [[nodiscard]] core::client::ActivityEventSequence nextEventSequence() noexcept;

    // 목적: 단일 active Folder scan에 사용할 adapter-local identity 발급
    // 입력: 없음
    // 출력: 다음 Folder scan activity identity
    [[nodiscard]] core::client::ActivityId nextFolderActivityId() noexcept;

    // 목적: accepted owner request를 active Activity 목록에 추가하고 snapshot publish
    // 입력: activity: kind, owner identity, cancellation capability와 optional Photo identity
    // 출력: active snapshot event queue 등록
    void recordStarted(core::client::ActiveActivity activity);

    // 목적: owner terminal 결과를 active 목록에서 제거하고 terminal snapshot publish
    // 입력: terminal: completed·failed·cancelled 결과와 optional error
    // 출력: terminal event를 생략하지 않는 snapshot queue 등록
    void recordTerminal(core::client::ActivityTerminal terminal);

    // 목적: 현재 active 목록과 optional terminal 결과를 모든 active subscription에 fan-out
    // 입력: terminal: 이번 transition의 optional terminal 결과
    // 출력: subscription별 Qt queued callback 등록
    void publishEvent(std::optional<core::client::ActivityTerminal> terminal);

    // 목적: 한 subscription에 immutable Activity event를 Qt queue로 등록
    // 입력: state: subscription lifetime, event: 전달할 snapshot event
    // 출력: 전달 불필요 또는 queue 성공이면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::ActivityEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 Activity event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state, const core::client::ActivityEvent& event) noexcept;

    facade::CatalogEditorFacade* m_catalogEditorFacade{nullptr};
    FolderScanController* m_folderScanController{nullptr};
    std::vector<core::client::ActiveActivity> m_activeActivities;
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::optional<core::client::ActivityId> m_folderActivityId;
    std::uint64_t m_nextEventSequence{1};
    std::uint64_t m_nextFolderActivityId{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::ui::mainwindow

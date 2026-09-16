#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "folder_import_client.h"

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
}

namespace flexraw::ui::facade
{

class QtFolderImportEventAdapter final : public QObject, public core::client::IFolderImportEventSource
{
public:
    // 목적: Catalog-owned Folder lifecycle을 현재 Qt thread의 serialized client event로 변환
    // 입력: catalogOrchestrator: authoritative Folder operation owner, parent: Qt lifetime owner
    // 출력: Qt delivery context에 bound된 event adapter
    explicit QtFolderImportEventAdapter(core::orchestration::CatalogOrchestrator& catalogOrchestrator,
                                        QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtFolderImportEventAdapter() override;

    // 목적: initial active Folder operation과 이후 lifecycle event를 Qt delivery context에서 구독
    // 입력: callback: immutable Folder operation event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::FolderOperationSubscriptionResult subscribeToFolderOperations(
        core::client::FolderOperationCallback callback) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    // 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
    // 입력: 없음
    // 출력: 다음 Folder operation event sequence
    [[nodiscard]] core::client::FolderOperationEventSequence nextEventSequence() noexcept;

    // 목적: accepted Folder operation을 current active state로 기록하고 publish
    // 입력: receipt: owner가 발급한 operation identity와 kind
    // 출력: active snapshot event queue 등록
    void recordStarted(const core::client::FolderOperationReceipt& receipt);

    // 목적: owner terminal을 기록하고 current active state에서 제거한 뒤 publish
    // 입력: terminal: completed·failed·cancelled exact terminal
    // 출력: terminal event queue 등록
    void recordTerminal(const core::client::FolderOperationTerminal& terminal);

    // 목적: 현재 active operation과 optional terminal을 모든 active subscription에 fan-out
    // 입력: terminal: 이번 transition의 optional terminal
    // 출력: subscription별 Qt queued callback 등록
    void publishEvent(std::optional<core::client::FolderOperationTerminal> terminal);

    // 목적: 한 subscription에 immutable Folder operation event를 Qt queue로 등록
    // 입력: state: subscription lifetime, event: 전달할 snapshot event
    // 출력: 전달 불필요 또는 queue 성공이면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::FolderOperationEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 Folder operation event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state,
                             const core::client::FolderOperationEvent& event) noexcept;

    core::orchestration::CatalogOrchestrator* m_catalogOrchestrator{nullptr};
    std::optional<core::client::FolderOperationReceipt> m_activeOperation;
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::uint64_t m_nextEventSequence{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::ui::facade

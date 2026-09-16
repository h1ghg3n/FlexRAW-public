#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "operation_types.h"
#include "photo_identity.h"
#include "source_resolution_client.h"

namespace flexraw::core::orchestration
{
class CatalogOrchestrator;
struct CatalogIssue;
struct CatalogSourceUpdate;

class QtSourceResolutionEventSource final : public QObject, public core::client::ISourceResolutionEventSource
{
public:
    // 목적: Catalog-owned source fingerprint lifecycle을 Qt-free Source Resolution event로 변환
    // 입력: catalogOrchestrator: authoritative request/mutation owner, parent: Qt lifetime owner
    // 출력: 현재 Qt delivery context에 bound된 event adapter
    explicit QtSourceResolutionEventSource(CatalogOrchestrator& catalogOrchestrator, QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtSourceResolutionEventSource() override;

    // 목적: initial active source requests와 이후 accepted·update·issue·terminal 구독
    // 입력: callback: immutable Source Resolution event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::SourceResolutionSubscriptionResult subscribeToSourceResolution(
        core::client::SourceResolutionCallback callback) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    // 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
    // 입력: 없음
    // 출력: 다음 Source Resolution event sequence
    [[nodiscard]] core::client::SourceResolutionEventSequence nextEventSequence() noexcept;

    // 목적: owner accepted request를 current active snapshot과 accepted event로 투영
    // 입력: requestId: owner identity, photoId: stable target identity
    // 출력: request kind·locator가 포함된 queued event
    void recordStarted(core::types::RequestId requestId, core::types::PhotoId photoId);

    // 목적: repository source transition을 immutable photo update와 completed terminal로 투영
    // 입력: update: request identity와 갱신된 기존·optional 신규 photo
    // 출력: active request가 제거된 update/terminal event
    void recordUpdated(const CatalogSourceUpdate& update);

    // 목적: fingerprint 또는 repository failure를 issue와 failed terminal로 투영
    // 입력: issue: request·Photo identity와 technical error
    // 출력: active request가 제거된 issue/terminal event
    void recordFailed(const CatalogIssue& issue);

    // 목적: owner cancellation을 exact cancelled terminal로 투영
    // 입력: requestId: 취소된 owner request identity
    // 출력: active request가 제거된 terminal event
    void recordCancelled(core::types::RequestId requestId);

    // 목적: 이전 active snapshot에서 terminal request context 조회
    // 입력: requestId: completed·failed·cancelled owner identity
    // 출력: kind·Photo·locator receipt 또는 찾을 수 없으면 빈 값
    [[nodiscard]] std::optional<core::client::SourceRequestReceipt> findActiveRequest(
        core::types::RequestId requestId) const;

    // 목적: current snapshot과 transition payload를 모든 active subscription에 fan-out
    // 입력: accepted/update/issue/terminal: 이번 source lifecycle transition payload
    // 출력: subscription별 Qt queued callback 등록
    void publishEvent(std::optional<core::client::SourceRequestReceipt> accepted,
                      std::optional<core::client::SourceResolutionUpdate> update,
                      std::optional<core::client::SourceResolutionIssue> issue,
                      std::optional<core::client::SourceResolutionTerminal> terminal);

    // 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
    // 입력: state: subscription lifetime, event: 전달할 Source Resolution event
    // 출력: 전달 불필요 또는 queue 성공이면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::SourceResolutionEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 Source Resolution event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state,
                             const core::client::SourceResolutionEvent& event) noexcept;

    CatalogOrchestrator* m_catalogOrchestrator{nullptr};
    core::client::SourceResolutionSnapshot m_snapshot;
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::uint64_t m_nextEventSequence{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::core::orchestration

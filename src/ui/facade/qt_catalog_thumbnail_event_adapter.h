#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "catalog_thumbnail_client.h"

namespace flexraw::core::orchestration
{
class CatalogThumbnailOrchestrator;
struct CatalogThumbnailFrame;
struct CatalogThumbnailIssue;
struct CatalogThumbnailWindowStarted;
struct CatalogThumbnailWindowTerminal;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::facade
{

class QtCatalogThumbnailEventAdapter final : public QObject, public core::client::ICatalogThumbnailEventSource
{
public:
    // 목적: Catalog thumbnail owner lifecycle을 Qt-free snapshot/event로 변환
    // 입력: orchestrator: authoritative window/generation owner, parent: Qt lifetime owner
    // 출력: 현재 Qt delivery context에 bound된 adapter
    explicit QtCatalogThumbnailEventAdapter(core::orchestration::CatalogThumbnailOrchestrator& orchestrator,
                                            QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtCatalogThumbnailEventAdapter() override;

    // 목적: Qt GUI delivery context에서 initial thumbnail snapshot과 이후 lifecycle 구독
    // 입력: callback: immutable frame·issue·terminal consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::CatalogThumbnailSubscriptionResult subscribeToCatalogThumbnails(
        core::client::CatalogThumbnailCallback callback) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    // 목적: adapter lifetime에서 0을 사용하지 않는 단조 event sequence 발급
    // 입력: 없음
    // 출력: 다음 Catalog thumbnail event sequence
    [[nodiscard]] core::client::CatalogThumbnailEventSequence nextEventSequence() noexcept;

    // 목적: owner가 accepted한 thumbnail window를 current snapshot과 started event로 투영
    // 입력: started: generation, accepted item 수와 target extent
    // 출력: 최신 snapshot을 가진 queued event
    void recordStarted(const core::orchestration::CatalogThumbnailWindowStarted& started);

    // 목적: owner가 stale filtering한 QImage frame을 immutable DisplayFrame event로 투영
    // 입력: frame: generation, tagged identity와 decoded image
    // 출력: frame 또는 projection issue event
    void recordFrame(const core::orchestration::CatalogThumbnailFrame& frame);

    // 목적: item decode 실패를 공통 ClientError issue로 투영
    // 입력: issue: generation, tagged identity와 CoreError
    // 출력: immutable issue event
    void recordIssue(const core::orchestration::CatalogThumbnailIssue& issue);

    // 목적: owner exact terminal을 Qt-free terminal state로 투영
    // 입력: terminal: generation과 optional technical error
    // 출력: active generation이 제거된 terminal event
    void recordTerminal(const core::orchestration::CatalogThumbnailWindowTerminal& terminal);

    // 목적: current snapshot과 transition payload를 모든 active subscription에 fan-out
    // 입력: windowStarted/frame/issue/terminal: 이번 transition 종류와 payload
    // 출력: subscription별 Qt queued callback 등록
    void publishEvent(bool windowStarted,
                      std::optional<core::client::CatalogThumbnailFrameSnapshot> frame,
                      std::optional<core::client::CatalogThumbnailIssue> issue,
                      std::optional<core::client::CatalogThumbnailTerminal> terminal);

    // 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
    // 입력: state: subscription lifetime, event: 전달할 Catalog thumbnail event
    // 출력: 전달 불필요 또는 queue 성공이면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::CatalogThumbnailEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 Catalog thumbnail event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state,
                             const core::client::CatalogThumbnailEvent& event) noexcept;

    core::orchestration::CatalogThumbnailOrchestrator* m_orchestrator{nullptr};
    core::client::CatalogThumbnailWindowSnapshot m_snapshot;
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::uint64_t m_nextEventSequence{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::ui::facade

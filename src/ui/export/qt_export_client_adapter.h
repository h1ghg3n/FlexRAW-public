#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QObject>

#include "export_client.h"

namespace flexraw::core::orchestration
{
class ExportOrchestrator;
struct ExportIssue;
struct ExportProgress;
struct ExportResult;
}  // namespace flexraw::core::orchestration

namespace flexraw::ui::export_
{

class QtExportClientAdapter final : public QObject,
                                    public core::client::IExportClient,
                                    public core::client::IExportEventSource
{
public:
    // 목적: application-scoped Export owner와 Worker profile resource를 Qt-free product surface에 연결
    // 입력: orchestrator: scheduling/terminal authority, workerProfileClient: endpoint resolution source, parent: Qt
    // owner 출력: current Qt delivery context에 bound된 Export adapter
    QtExportClientAdapter(core::orchestration::ExportOrchestrator& orchestrator,
                          core::client::IWorkerProfileClient& workerProfileClient,
                          QObject* parent = nullptr);

    // 목적: queued callback을 차단하고 outliving subscription handle을 inactive 상태로 전환
    // 입력: 없음
    // 출력: adapter destruction 이후 callback 없음
    ~QtExportClientAdapter() override;

    // 목적: Qt-free Export command를 현재 Orchestrator request와 resolved Worker target으로 투영
    // 입력: command: file·item-list·batch request와 placement/profile identity
    // 출력: accepted request receipt 또는 validation·profile·owner 오류
    [[nodiscard]] core::client::ExportSubmissionResult submitExport(
        const core::client::SubmitExportCommand& command) override;

    // 목적: accepted Export request cancellation을 authoritative owner에 전달
    // 입력: requestId: Qt-free Export request identity
    // 출력: 취소 수락 identity 또는 stale·validation 오류
    [[nodiscard]] core::client::ExportCancellationResult cancelExport(core::client::ExportRequestId requestId) override;

    // 목적: 전체 또는 지정 active Export request와 lifetime scheduling metric 조회
    // 입력: requestId: optional active request identity
    // 출력: immutable snapshot 또는 stale·thread 오류
    [[nodiscard]] core::client::ExportSnapshotResult exportSnapshot(
        std::optional<core::client::ExportRequestId> requestId = std::nullopt) const override;

    // 목적: initial active snapshot과 이후 accepted·progress·terminal lifecycle 구독
    // 입력: callback: immutable Export event consumer
    // 출력: RAII unsubscribe handle 또는 callback·thread 오류
    [[nodiscard]] core::client::ExportSubscriptionResult subscribeToExports(
        core::client::ExportCallback callback) override;

private:
    struct SubscriptionState;
    class Subscription;
    using SubscriptionStatePtr = std::shared_ptr<SubscriptionState>;

    // 목적: adapter lifetime에서 0을 사용하지 않는 event ordering sequence 발급
    // 입력: 없음
    // 출력: 다음 Export event sequence
    [[nodiscard]] core::client::ExportEventSequence nextEventSequence() noexcept;

    // 목적: owner가 accepted한 request를 active snapshot과 event로 투영
    // 입력: requestId: authoritative aggregate identity
    // 출력: active request 등록과 accepted event
    void recordAccepted(std::uint64_t requestId);

    // 목적: owner 누적 progress를 active snapshot과 event로 투영
    // 입력: progress: request identity와 item 누적 상태
    // 출력: matching active request 갱신과 progress event
    void recordProgress(const core::orchestration::ExportProgress& progress);

    // 목적: owner completed report를 Qt-free exact terminal event로 투영
    // 입력: result: item report와 request scheduling metric
    // 출력: active request 제거와 completed event
    void recordCompleted(const core::orchestration::ExportResult& result);

    // 목적: owner request-level failure를 ClientError exact terminal로 투영
    // 입력: issue: request identity와 CoreError
    // 출력: active request 제거와 failed event
    void recordFailed(const core::orchestration::ExportIssue& issue);

    // 목적: owner cancellation을 Qt-free exact terminal event로 투영
    // 입력: requestId: cancelled aggregate identity
    // 출력: active request 제거와 cancelled event
    void recordCancelled(std::uint64_t requestId);

    // 목적: active request vector에서 identity가 일치하는 mutable snapshot 조회
    // 입력: requestId: 찾을 aggregate identity
    // 출력: matching snapshot pointer 또는 nullptr
    [[nodiscard]] core::client::ActiveExportSnapshot* findActive(core::client::ExportRequestId requestId) noexcept;

    // 목적: terminal request를 active snapshot에서 제거
    // 입력: requestId: 제거할 aggregate identity
    // 출력: snapshot에 해당 identity가 남지 않음
    void removeActive(core::client::ExportRequestId requestId);

    // 목적: current snapshot과 transition payload를 모든 active subscription에 fan-out
    // 입력: accepted/progress/completed/failed/cancelled: 이번 lifecycle transition payload
    // 출력: subscription별 Qt queued callback 등록
    void publishEvent(std::optional<core::client::ExportRequestReceipt> accepted,
                      std::optional<core::client::ExportProgress> progress,
                      std::optional<core::client::ExportResult> completed,
                      std::optional<core::client::ExportIssue> failed,
                      std::optional<core::client::ExportCancellation> cancelled);

    // 목적: 한 subscription에 immutable event를 Qt queued callback으로 등록
    // 입력: state: subscription lifetime, event: 전달할 Export event
    // 출력: 전달 불필요 또는 queue 성공이면 true
    [[nodiscard]] bool enqueueEvent(const SubscriptionStatePtr& state, core::client::ExportEvent event);

    // 목적: queued immutable event를 active callback 하나에 예외 격리하여 전달
    // 입력: state: subscription lifetime, event: 전달할 Export event
    // 출력: 없음
    static void deliverEvent(const SubscriptionStatePtr& state, const core::client::ExportEvent& event) noexcept;

    core::orchestration::ExportOrchestrator* m_orchestrator{nullptr};
    core::client::IWorkerProfileClient* m_workerProfileClient{nullptr};
    core::client::ExportSnapshot m_snapshot;
    std::vector<std::weak_ptr<SubscriptionState>> m_subscriptions;
    std::uint64_t m_nextEventSequence{1};
    bool m_shuttingDown{false};
};

}  // namespace flexraw::ui::export_

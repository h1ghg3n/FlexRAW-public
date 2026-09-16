#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <QHash>
#include <QList>
#include <QObject>
#include <QThreadPool>
#include <QTimer>

#include "export_execution_port.h"

namespace flexraw::platform
{
class ISystemMemoryProbe;
}

namespace flexraw::core::orchestration
{

struct ExportSchedulingConfiguration
{
    int localSlotLimit{2};
    int remoteSlotLimit{1};
    int maximumRemoteDispatchAttempts{3};
    bool enforceLocalResourceReserve{false};
    int reservedLogicalProcessors{4};
    std::uint64_t memoryReserveBytes{10ULL * 1024ULL * 1024ULL * 1024ULL};
    std::uint64_t memoryClaimPerJobBytes{1024ULL * 1024ULL * 1024ULL};
    std::chrono::milliseconds serverBusyCooldown{2000};
    std::chrono::milliseconds resourceBusyCooldown{3000};
    std::chrono::milliseconds connectionFailedCooldown{5000};
};

class ExportOrchestrator final : public QObject
{
    Q_OBJECT

public:
    // 목적: Local-only compatibility용 application-scoped export scheduler 조립
    // 입력: pipeline: thread-safe pipeline, maximumConcurrentJobs: Local slot 수, parent: Qt parent 객체
    // 출력: 기존 caller와 호환되는 ExportOrchestrator 객체
    explicit ExportOrchestrator(std::unique_ptr<IExportPipeline> pipeline,
                                int maximumConcurrentJobs = 0,
                                QObject* parent = nullptr);

    // 목적: Local/Remote execution port와 resource-aware scheduling policy 조립
    // 입력: pipeline/remotePort/memoryProbe: injected dependency, configuration: slot/retry/reserve 정책
    // 출력: LocalOnly/RemoteOnly/Auto 실행이 가능한 ExportOrchestrator 객체
    ExportOrchestrator(std::unique_ptr<IExportPipeline> pipeline,
                       std::unique_ptr<IRemoteExportExecutionPort> remotePort,
                       const platform::ISystemMemoryProbe* memoryProbe,
                       ExportSchedulingConfiguration configuration,
                       QObject* parent = nullptr);

    // 목적: 새 request를 차단하고 active job을 취소한 뒤 모든 worker pool 종료 대기
    // 입력: 없음
    // 출력: 없음
    ~ExportOrchestrator() override;

    // 목적: export request를 검증하고 placement별 item scheduling lifecycle 생성
    // 입력: request: file 또는 batch 요청, placement: LocalOnly/RemoteOnly/Auto와 optional target
    // 출력: 수락된 RequestId 또는 즉시 validation 오류
    [[nodiscard]] ExportSubmissionResult submitExport(ExportRequest request, ExportPlacementOptions placement = {});

    // 목적: 지정 export request의 queued/running item을 terminal cancellation으로 전환
    // 입력: requestId: 취소할 export request 식별자
    // 출력: active request를 취소했으면 true
    bool cancelExport(types::RequestId requestId);

    // 목적: application lifetime 동안 누적된 placement metric snapshot 반환
    // 입력: 없음
    // 출력: item terminal/dispatch/rejection 누적 counter
    [[nodiscard]] ExportSchedulingMetrics schedulingMetrics() const;

signals:
    // 목적: validation을 통과하고 owner state에 등록된 Export request identity 전달
    // 입력: requestId: accepted aggregate request identity
    // 출력: 없음
    void exportAccepted(types::RequestId requestId);

    // 목적: active export request의 누적 진행 상태 전달
    // 입력: progress: request identity, 완료/성공/실패 수와 현재 source
    // 출력: 없음
    void exportProgressed(const ExportProgress& progress);

    // 목적: export workflow가 item별 상세 report와 함께 종료됐음을 전달
    // 입력: result: request identity와 전체 item 결과
    // 출력: 없음
    void exportCompleted(const ExportResult& result);

    // 목적: export workflow 준비 또는 scheduler-level terminal failure 전달
    // 입력: issue: request identity와 technical error
    // 출력: 없음
    void exportFailed(const ExportIssue& issue);

    // 목적: accepted export request의 terminal cancellation 전달
    // 입력: requestId: 취소된 request 식별자
    // 출력: 없음
    void exportCancelled(types::RequestId requestId);

private:
    using ExportJobId = std::uint64_t;

    enum class JobState : std::uint8_t
    {
        Queued,
        DispatchingRemote,
        RunningLocal,
        RunningRemote,
        Cancelled,
    };

    enum class JobOwner : std::uint8_t
    {
        Local,
        Remote,
    };

    struct ActiveExportRequest
    {
        types::CancellationSource cancellationSource;
        ExportPlacementOptions placement;
        ExportReport report;
        ExportSchedulingMetrics scheduling;
        int completedCount{0};
        int localInFlight{0};
        int requestedLocalLimit{0};
        bool preparing{true};
        bool cancellationPublished{false};
    };

    struct ExportJob
    {
        ExportJobId jobId{0};
        types::RequestId requestId{0};
        std::uint64_t enqueueSequence{0};
        int itemIndex{0};
        PreparedExportItem item;
        JobState state{JobState::Queued};
        std::optional<JobOwner> owner;
        int localAttempts{0};
        int remoteAttempts{0};
        int dispatchRejections{0};
        std::chrono::steady_clock::time_point remoteDeferUntil{};
        bool remoteEligible{true};
        bool remoteAccepted{false};
    };

    // 목적: request preparation을 전용 worker에서 시작
    // 입력: requestId: aggregate identity, request: immutable file 또는 batch 값
    // 출력: queued preparation task
    void startPreparation(types::RequestId requestId, ExportRequest request);

    // 목적: preparation 결과를 item queue 또는 request terminal event로 변환
    // 입력: requestId: aggregate identity, result: resolved item 목록 또는 준비 오류
    // 출력: pending job 생성과 scheduling pump 실행 가능
    void handlePrepared(types::RequestId requestId, ExportPreparationResult result);

    // 목적: Remote reservation을 먼저 수행하고 남은 Local slot을 채움
    // 입력: 없음
    // 출력: bounded worker task 제출과 wakeup timer 갱신
    void pump();

    // 목적: stable enqueue order에서 현재 Remote dispatch 가능한 oldest job 탐색
    // 입력: now: cooldown 비교용 monotonic 시각
    // 출력: candidate JobId 또는 없음
    [[nodiscard]] std::optional<ExportJobId> findRemoteCandidate(std::chrono::steady_clock::time_point now) const;

    // 목적: stable enqueue order에서 현재 Local slot을 받을 oldest job 탐색
    // 입력: 없음
    // 출력: candidate JobId 또는 없음
    [[nodiscard]] std::optional<ExportJobId> findLocalCandidate() const;

    // 목적: Queued item 하나를 Local owner에게 제출
    // 입력: jobId: claim할 pending job
    // 출력: Local slot/attempt 증가와 background item 실행
    void startLocal(ExportJobId jobId);

    // 목적: Queued item 하나를 직렬 Remote handshake에 제출
    // 입력: jobId: reserve할 pending job
    // 출력: DispatchingRemote 전환과 background adapter 실행
    void startRemote(ExportJobId jobId);

    // 목적: Local worker 결과의 slot과 request aggregate 갱신
    // 입력: jobId: internal item identity, result: item success/failure
    // 출력: terminal item 또는 cancellation cleanup 후 pump 실행
    void handleLocalFinished(ExportJobId jobId, ExportItemResult result);

    // 목적: 유효한 JobAccepted를 Remote ownership과 slot으로 반영
    // 입력: jobId: dispatch 중인 item identity
    // 출력: handshake 해제, RunningRemote 전환과 다음 pump
    void handleRemoteAccepted(ExportJobId jobId);

    // 목적: normalized Remote 결과를 retry/fallback/terminal policy로 해석
    // 입력: jobId: internal item identity, result: adapter success 또는 typed failure
    // 출력: Remote slot 해제와 item requeue/terminal 처리
    void handleRemoteFinished(ExportJobId jobId, RemoteExportExecutionResult result);

    // 목적: NotStarted Remote rejection을 cooldown과 attempt 제한을 적용해 재등록
    // 입력: jobId: 대상 item, delay: Remote target 재허용 지연, cause: exhaustion 진단
    // 출력: Auto requeue 또는 RemoteOnly/attempt exhaustion terminal 처리
    void requeueRemote(ExportJobId jobId, std::chrono::milliseconds delay, const types::CoreError& cause);

    // 목적: item 결과를 request report와 lifetime metric에 정확히 한 번 반영
    // 입력: jobId: terminal item, result: 성공/실패와 failure kind
    // 출력: progress 및 request completion event 가능
    void completeJob(ExportJobId jobId, ExportItemResult result);

    // 목적: internal job을 queue/hash에서 제거
    // 입력: jobId: 제거할 item identity
    // 출력: enqueue order와 job storage 동시 정리
    void removeJob(ExportJobId jobId);

    // 목적: 모든 item이 끝난 request를 report와 함께 완료
    // 입력: requestId: aggregate identity
    // 출력: exportCompleted signal 또는 cancelled request cleanup
    void finishRequestIfReady(types::RequestId requestId);

    // 목적: cancelled request의 background cleanup 완료 여부 확인
    // 입력: requestId: cancellation을 이미 publish한 aggregate identity
    // 출력: 남은 preparation/job이 없으면 request storage 제거
    void cleanupCancelledRequest(types::RequestId requestId);

    // 목적: request에 속한 internal job 존재 여부 검사
    // 입력: requestId: aggregate identity
    // 출력: queued/running/cancel cleanup item이 하나라도 있으면 true
    [[nodiscard]] bool hasJobs(types::RequestId requestId) const;

    // 목적: 현재 host resource reserve를 반영한 Local hard ceiling 계산
    // 입력: 없음
    // 출력: 새 Local item을 포함해 허용할 total Local in-flight 수
    [[nodiscard]] int availableLocalSlotLimit() const;

    // 목적: cooldown/resource wait 중인 pending item을 위한 non-blocking wakeup 예약
    // 입력: 없음
    // 출력: earliest cooldown 또는 resource 재확인 timer 설정
    void scheduleWakeups();

    // 목적: request와 lifetime metric의 동일 counter를 함께 증가
    // 입력: requestId: aggregate identity, member: 증가할 metric field, amount: 증가량
    // 출력: request/lifetime snapshot 갱신
    void incrementMetric(types::RequestId requestId,
                         std::uint64_t ExportSchedulingMetrics::* member,
                         std::uint64_t amount = 1);

    std::unique_ptr<IExportPipeline> m_pipeline;
    std::unique_ptr<IRemoteExportExecutionPort> m_remotePort;
    const platform::ISystemMemoryProbe* m_memoryProbe{nullptr};
    ExportSchedulingConfiguration m_configuration;
    QThreadPool m_preparationPool;
    QThreadPool m_localPool;
    QThreadPool m_remotePool;
    QTimer m_cooldownTimer;
    QTimer m_resourceTimer;
    QHash<types::RequestId, ActiveExportRequest> m_activeRequests;
    QHash<ExportJobId, ExportJob> m_jobs;
    QList<ExportJobId> m_enqueueOrder;
    ExportSchedulingMetrics m_lifetimeMetrics;
    std::optional<ExportJobId> m_remoteDispatchJobId;
    types::RequestId m_nextRequestId{1};
    ExportJobId m_nextJobId{1};
    int m_localInFlight{0};
    int m_remoteInFlight{0};
    bool m_acceptingRequests{true};
    bool m_pumpActive{false};
};

}  // namespace flexraw::core::orchestration

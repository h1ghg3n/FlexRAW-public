#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <QHash>
#include <QObject>
#include <QSize>
#include <QThreadPool>
#include <QVector>

#include "catalog_thumbnail_contracts.h"
#include "catalog_thumbnail_pipeline.h"
#include "file_types.h"
#include "operation_types.h"

template<typename T> class QFutureWatcher;

namespace flexraw::core::orchestration
{

class CatalogThumbnailOrchestrator final : public QObject, public client::ICatalogThumbnailClient
{
    Q_OBJECT

public:
    // 목적: viewport thumbnail을 직렬 background decode하는 application-scoped Orchestrator 생성
    // 입력: pipeline: worker에서 사용할 synchronous thumbnail pipeline, parent: Qt 부모 object
    // 출력: bounded pending window를 가진 Orchestrator 객체
    explicit CatalogThumbnailOrchestrator(std::unique_ptr<ICatalogThumbnailPipeline> pipeline,
                                          QObject* parent = nullptr);

    // 목적: active thumbnail decode를 취소하고 worker resource 정리
    // 입력: 없음
    // 출력: pending/active thumbnail 작업이 남지 않음
    ~CatalogThumbnailOrchestrator() override;

    // 목적: 현재 viewport와 인접 범위의 Qt-free tagged thumbnail window 교체
    // 입력: command: 최대 200개 item과 양수 target pixel 크기
    // 출력: owner generation과 중복 제거 item 수 또는 validation·shutdown 오류
    [[nodiscard]] client::CatalogThumbnailWindowResult replaceThumbnailWindow(
        const client::ReplaceCatalogThumbnailWindowCommand& command) override;

    // 목적: active thumbnail window와 pending decode를 idempotent하게 정리
    // 입력: 없음
    // 출력: active generation이 있으면 Cancelled terminal을 발행한 성공 또는 shutdown 오류
    [[nodiscard]] client::CatalogThumbnailClearResult clearThumbnailWindow() override;

    // 목적: event adapter initial delivery에 사용할 current thumbnail lifecycle 조회
    // 입력: 없음
    // 출력: active generation, target과 요청·settled item count
    [[nodiscard]] client::CatalogThumbnailWindowSnapshot thumbnailWindowSnapshot() const noexcept;

signals:
    // 목적: owner가 accepted한 새 thumbnail window context 전달
    // 입력: started: generation, item 수와 target extent
    // 출력: 없음
    void thumbnailWindowStarted(const CatalogThumbnailWindowStarted& started);

    // 목적: 현재 thumbnail window에 속하는 decode frame 전달
    // 입력: frame: source path와 목록용 image
    // 출력: 없음
    void thumbnailReady(const CatalogThumbnailFrame& frame);

    // 목적: 현재 thumbnail window source의 terminal decode 실패 전달
    // 입력: issue: source path와 구조화된 오류
    // 출력: 없음
    void thumbnailFailed(const CatalogThumbnailIssue& issue);

    // 목적: accepted thumbnail window의 exact terminal 전달
    // 입력: terminal: generation과 Completed/Failed/Cancelled 상태
    // 출력: 없음
    void thumbnailWindowTerminal(const CatalogThumbnailWindowTerminal& terminal);

private:
    struct PendingJob
    {
        client::CatalogThumbnailWindowGeneration generation;
        client::CatalogThumbnailItemIdentity identity;
        types::FileDescriptor source;
        QSize targetSize;
    };

    struct ActiveJob
    {
        PendingJob request;
        types::CancellationSource cancellationSource;
        QFutureWatcher<CatalogThumbnailPipelineResult>* watcher{nullptr};
    };

    struct ActiveWindow
    {
        client::CatalogThumbnailWindowGeneration generation;
        client::CatalogThumbnailTargetExtent targetExtent;
        std::uint32_t requestedItemCount{0};
        std::uint32_t settledItemCount{0};
    };

    // 목적: worker slot이 비어 있으면 최신 window의 다음 thumbnail decode 시작
    // 입력: 없음
    // 출력: active 작업 수가 concurrency 상한 이내로 유지됨
    void startNextJob();

    // 목적: background decode 결과를 stale filtering 후 terminal signal로 변환
    // 입력: requestId: 완료된 active thumbnail 작업 identity
    // 출력: current window면 ready/failed signal 하나 발생 후 다음 작업 시작
    void handleJobFinished(types::RequestId requestId);

    // 목적: current thumbnail window의 pending/active 작업 취소와 exact terminal 확정
    // 입력: 없음
    // 출력: active window가 없고 stale worker 결과 publish가 차단됨
    void cancelCurrentWindow();

    // 목적: current window의 모든 item이 settled되면 Completed terminal 확정
    // 입력: 없음
    // 출력: 완료 조건이면 active window가 제거되고 terminal signal 발생
    void completeCurrentWindowIfSettled();

    // 목적: active decode에 cooperative cancellation 요청
    // 입력: 없음
    // 출력: 향후 stale frame publish가 차단됨
    void cancelActiveJobs();

    std::unique_ptr<ICatalogThumbnailPipeline> m_pipeline;
    QThreadPool m_workerPool;
    QVector<PendingJob> m_pendingJobs;
    QHash<types::RequestId, ActiveJob> m_activeJobs;
    types::RequestId m_nextRequestId{1};
    std::uint64_t m_windowRevision{0};
    std::optional<ActiveWindow> m_activeWindow;
    bool m_acceptingRequests{true};
};

}  // namespace flexraw::core::orchestration

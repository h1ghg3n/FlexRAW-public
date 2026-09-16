#pragma once

#include <memory>

#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <QVector>

#include "catalog_thumbnail_contracts.h"
#include "catalog_thumbnail_pipeline.h"
#include "operation_types.h"

template<typename T> class QFutureWatcher;

namespace flexraw::core::orchestration
{

class CatalogThumbnailOrchestrator final : public QObject
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

    // 목적: 현재 viewport와 인접 범위에 필요한 source set으로 pending thumbnail window 교체
    // 입력: request: 최대 200개 source와 thumbnail target 크기
    // 출력: 수락 성공 또는 validation·shutdown 오류
    [[nodiscard]] CatalogThumbnailWindowResult updateWindow(CatalogThumbnailWindowRequest request);

signals:
    // 목적: 현재 thumbnail window에 속하는 decode frame 전달
    // 입력: frame: source path와 목록용 image
    // 출력: 없음
    void thumbnailReady(const CatalogThumbnailFrame& frame);

    // 목적: 현재 thumbnail window source의 terminal decode 실패 전달
    // 입력: issue: source path와 구조화된 오류
    // 출력: 없음
    void thumbnailFailed(const CatalogThumbnailIssue& issue);

private:
    struct PendingJob
    {
        quint64 windowRevision{0};
        types::FileDescriptor source;
        QSize targetSize;
    };

    struct ActiveJob
    {
        PendingJob request;
        types::CancellationSource cancellationSource;
        QFutureWatcher<CatalogThumbnailPipelineResult>* watcher{nullptr};
    };

    // 목적: worker slot이 비어 있으면 최신 window의 다음 thumbnail decode 시작
    // 입력: 없음
    // 출력: active 작업 수가 concurrency 상한 이내로 유지됨
    void startNextJob();

    // 목적: background decode 결과를 stale filtering 후 terminal signal로 변환
    // 입력: requestId: 완료된 active thumbnail 작업 identity
    // 출력: current window면 ready/failed signal 하나 발생 후 다음 작업 시작
    void handleJobFinished(types::RequestId requestId);

    // 목적: active decode에 cooperative cancellation 요청
    // 입력: 없음
    // 출력: 향후 stale frame publish가 차단됨
    void cancelActiveJobs();

    std::unique_ptr<ICatalogThumbnailPipeline> m_pipeline;
    QThreadPool m_workerPool;
    QVector<PendingJob> m_pendingJobs;
    QHash<types::RequestId, ActiveJob> m_activeJobs;
    types::RequestId m_nextRequestId{1};
    quint64 m_windowRevision{0};
    bool m_acceptingRequests{true};
};

}  // namespace flexraw::core::orchestration

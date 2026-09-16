#include "catalog_thumbnail_orchestrator.h"

#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include <QFutureWatcher>
#include <QSet>
#include <QtConcurrentRun>

namespace flexraw::core::orchestration
{
namespace
{

// 목적: thumbnail window request의 크기, target과 source 값 검증
// 입력: request: UI adapter가 제출한 viewport source set
// 출력: 유효하면 빈 값, 아니면 InvalidArgument 오류
[[nodiscard]] std::optional<types::CoreError> validateWindowRequest(const CatalogThumbnailWindowRequest& request)
{
    if (request.sources.size() > MaximumCatalogThumbnailWindowSize)
    {
        return types::CoreError{types::ErrorCode::InvalidArgument,
                                QStringLiteral("Catalog thumbnail window exceeds its bounded size.")};
    }
    if (!request.sources.isEmpty() && (request.targetSize.width() <= 0 || request.targetSize.height() <= 0))
    {
        return types::CoreError{types::ErrorCode::InvalidArgument,
                                QStringLiteral("Catalog thumbnail target size must be positive.")};
    }
    for (const types::FileDescriptor& source : request.sources)
    {
        if (source.path.isEmpty() ||
            (source.kind != types::SupportedFileKind::Raw && source.kind != types::SupportedFileKind::RasterImage))
        {
            return types::CoreError{types::ErrorCode::InvalidArgument,
                                    QStringLiteral("Catalog thumbnail source is invalid.")};
        }
    }
    return std::nullopt;
}

}  // namespace

// 목적: viewport thumbnail을 직렬 background decode하는 application-scoped Orchestrator 생성
// 입력: pipeline: worker에서 사용할 synchronous thumbnail pipeline, parent: Qt 부모 object
// 출력: bounded pending window를 가진 Orchestrator 객체
CatalogThumbnailOrchestrator::CatalogThumbnailOrchestrator(std::unique_ptr<ICatalogThumbnailPipeline> pipeline,
                                                           QObject* parent)
    : QObject(parent), m_pipeline(std::move(pipeline))
{
    if (m_pipeline == nullptr)
    {
        throw std::invalid_argument("Catalog thumbnail pipeline must not be null.");
    }
    qRegisterMetaType<CatalogThumbnailFrame>();
    qRegisterMetaType<CatalogThumbnailIssue>();
    m_workerPool.setObjectName(QStringLiteral("CatalogThumbnailPool"));
    m_workerPool.setMaxThreadCount(1);
}

// 목적: active thumbnail decode를 취소하고 worker resource 정리
// 입력: 없음
// 출력: pending/active thumbnail 작업이 남지 않음
CatalogThumbnailOrchestrator::~CatalogThumbnailOrchestrator()
{
    m_acceptingRequests = false;
    ++m_windowRevision;
    m_pendingJobs.clear();
    cancelActiveJobs();
    m_workerPool.clear();
    m_workerPool.waitForDone();
    for (ActiveJob& job : m_activeJobs)
    {
        delete job.watcher;
        job.watcher = nullptr;
    }
    m_activeJobs.clear();
}

// 목적: 현재 viewport와 인접 범위에 필요한 source set으로 pending thumbnail window 교체
// 입력: request: 최대 200개 source와 thumbnail target 크기
// 출력: 수락 성공 또는 validation·shutdown 오류
CatalogThumbnailWindowResult CatalogThumbnailOrchestrator::updateWindow(CatalogThumbnailWindowRequest request)
{
    if (!m_acceptingRequests)
    {
        return CatalogThumbnailWindowResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Catalog thumbnail orchestrator is shutting down.")});
    }
    if (const std::optional<types::CoreError> error = validateWindowRequest(request); error.has_value())
    {
        return CatalogThumbnailWindowResult::failure(*error);
    }
    if (m_windowRevision == std::numeric_limits<quint64>::max())
    {
        return CatalogThumbnailWindowResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Catalog thumbnail window revision space is exhausted.")});
    }

    ++m_windowRevision;
    cancelActiveJobs();
    m_pendingJobs.clear();
    QSet<QString> acceptedPaths;
    for (types::FileDescriptor& source : request.sources)
    {
        if (!acceptedPaths.contains(source.path))
        {
            acceptedPaths.insert(source.path);
            m_pendingJobs.push_back({m_windowRevision, std::move(source), request.targetSize});
        }
    }
    startNextJob();
    return CatalogThumbnailWindowResult::success(std::monostate{});
}

// 목적: worker slot이 비어 있으면 최신 window의 다음 thumbnail decode 시작
// 입력: 없음
// 출력: active 작업 수가 concurrency 상한 이내로 유지됨
void CatalogThumbnailOrchestrator::startNextJob()
{
    if (!m_acceptingRequests || !m_activeJobs.isEmpty() || m_pendingJobs.isEmpty())
    {
        return;
    }
    if (m_nextRequestId == std::numeric_limits<types::RequestId>::max())
    {
        m_nextRequestId = 1;
    }

    const types::RequestId requestId = m_nextRequestId++;
    PendingJob pending = m_pendingJobs.takeFirst();
    ActiveJob active;
    active.request = pending;
    active.watcher = new QFutureWatcher<CatalogThumbnailPipelineResult>(this);
    const types::CancellationToken token = active.cancellationSource.token();
    QFutureWatcher<CatalogThumbnailPipelineResult>* watcher = active.watcher;
    m_activeJobs.insert(requestId, active);
    connect(watcher, &QFutureWatcher<CatalogThumbnailPipelineResult>::finished, this, [this, requestId] {
        handleJobFinished(requestId);
    });
    watcher->setFuture(QtConcurrent::run(&m_workerPool, [this, pending = std::move(pending), token] {
        return m_pipeline->load(pending.source, pending.targetSize, token);
    }));
}

// 목적: background decode 결과를 stale filtering 후 terminal signal로 변환
// 입력: requestId: 완료된 active thumbnail 작업 identity
// 출력: current window면 ready/failed signal 하나 발생 후 다음 작업 시작
void CatalogThumbnailOrchestrator::handleJobFinished(types::RequestId requestId)
{
    auto iterator = m_activeJobs.find(requestId);
    if (iterator == m_activeJobs.end())
    {
        return;
    }

    ActiveJob job = iterator.value();
    m_activeJobs.erase(iterator);
    const CatalogThumbnailPipelineResult result = job.watcher->result();
    job.watcher->deleteLater();
    if (job.request.windowRevision == m_windowRevision && !job.cancellationSource.token().isCancellationRequested())
    {
        if (result.hasValue())
        {
            emit thumbnailReady({job.request.source.path, result.value()});
        }
        else if (result.error().code != types::ErrorCode::Cancelled)
        {
            emit thumbnailFailed({job.request.source.path, result.error()});
        }
    }
    startNextJob();
}

// 목적: active decode에 cooperative cancellation 요청
// 입력: 없음
// 출력: 향후 stale frame publish가 차단됨
void CatalogThumbnailOrchestrator::cancelActiveJobs()
{
    for (ActiveJob& job : m_activeJobs)
    {
        job.cancellationSource.requestCancellation();
    }
}

}  // namespace flexraw::core::orchestration

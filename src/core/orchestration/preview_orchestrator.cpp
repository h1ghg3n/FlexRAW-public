#include "preview_orchestrator.h"

#include <chrono>
#include <limits>
#include <optional>
#include <utility>

#include <QMetaObject>
#include <QMetaType>

#include "log.h"
#include "preview_pipeline.h"
#include "preview_pipeline_worker.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: preview progression enum을 진단 가능한 안정적 이름으로 변환
// 입력: progression: request의 source tier 진행 정책
// 출력: log에 기록할 영문 이름
[[nodiscard]] const char* progressionName(PreviewProgression progression) noexcept
{
    switch (progression)
    {
    case PreviewProgression::Progressive:
        return "progressive";
    case PreviewProgression::FinalOnly:
        return "final-only";
    }

    return "unknown";
}

// 목적: preview render mode enum을 진단 가능한 안정적 이름으로 변환
// 입력: renderMode: interactive 또는 final render 정책
// 출력: log에 기록할 영문 이름
[[nodiscard]] const char* renderModeName(PreviewRenderMode renderMode) noexcept
{
    switch (renderMode)
    {
    case PreviewRenderMode::Interactive:
        return "interactive";
    case PreviewRenderMode::Final:
        return "final";
    }

    return "unknown";
}

// 목적: preview tier enum을 진단 가능한 안정적 이름으로 변환
// 입력: tier: publish된 source 품질 단계
// 출력: log에 기록할 영문 이름
[[nodiscard]] const char* tierName(PreviewTier tier) noexcept
{
    switch (tier)
    {
    case PreviewTier::Thumbnail:
        return "thumbnail";
    case PreviewTier::Standard:
        return "standard";
    }

    return "unknown";
}

// 목적: preview submission 기본 value contract 검증
// 입력: request: 검증할 preview 요청
// 출력: 유효하면 빈 error, 유효하지 않으면 구조화된 error
[[nodiscard]] std::optional<types::CoreError> validateRequest(const PreviewRequest& request)
{
    const bool hasCatalogIdentity = types::isValidPhotoId(request.photo.photoId);
    const bool hasTransientIdentity = !request.photo.transientKey.isEmpty();
    if (hasCatalogIdentity == hasTransientIdentity || request.source.path.isEmpty())
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Preview requires exactly one photo identity and a source path."),
        };
    }

    if (request.targetSize.width() <= 0 || request.targetSize.height() <= 0)
    {
        return types::CoreError{
            types::ErrorCode::InvalidArgument,
            QStringLiteral("Preview target size must be positive."),
        };
    }

    if (request.source.kind != types::SupportedFileKind::Raw &&
        request.source.kind != types::SupportedFileKind::RasterImage)
    {
        return types::CoreError{
            types::ErrorCode::UnsupportedFormat,
            QStringLiteral("Preview source kind is unsupported."),
        };
    }

    return std::nullopt;
}

}  // namespace

// 목적: application-scoped preview worker와 injected pipeline 조립
// 입력: pipeline: worker thread에서 소유할 synchronous pipeline, parent: Qt 부모 object
// 출력: 실행 가능한 Orchestrator 객체
PreviewOrchestrator::PreviewOrchestrator(std::unique_ptr<IPreviewPipeline> pipeline, QObject* parent)
    : QObject(parent), m_worker(new PreviewPipelineWorker(std::move(pipeline)))
{
    qRegisterMetaType<PreviewResult>();
    qRegisterMetaType<PreviewIssue>();
    m_workerThread.setObjectName(QStringLiteral("PreviewPipelineWorker"));
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &PreviewPipelineWorker::frameReady, this, &PreviewOrchestrator::handleFrameReady);
    connect(m_worker, &PreviewPipelineWorker::workWarning, this, &PreviewOrchestrator::handleWorkWarning);
    connect(m_worker, &PreviewPipelineWorker::workCompleted, this, &PreviewOrchestrator::handleWorkCompleted);
    connect(m_worker, &PreviewPipelineWorker::workFailed, this, &PreviewOrchestrator::handleWorkFailed);
    m_workerThread.start();
}

// 목적: 새 request 수락을 중단하고 active 작업 취소 후 worker 종료 대기
// 입력: 없음
// 출력: 없음
PreviewOrchestrator::~PreviewOrchestrator()
{
    m_acceptingRequests = false;

    for (const types::CancellationSource& cancellationSource : std::as_const(m_activeRequests))
    {
        cancellationSource.requestCancellation();
    }

    m_activeRequests.clear();
    m_workerThread.quit();
    m_workerThread.wait();
}

// 목적: preview request를 검증하고 고유 ID와 cancellation state를 부여해 background queue에 제출
// 입력: request: photo identity, source, params와 target을 포함한 immutable 요청
// 출력: 수락된 RequestId 또는 즉시 validation 오류
PreviewSubmissionResult PreviewOrchestrator::submitPreview(PreviewRequest request)
{
    if (!m_acceptingRequests)
    {
        return PreviewSubmissionResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Preview orchestrator is shutting down.")});
    }

    if (const std::optional<types::CoreError> error = validateRequest(request); error.has_value())
    {
        return PreviewSubmissionResult::failure(*error);
    }

    if (m_nextRequestId == std::numeric_limits<types::RequestId>::max())
    {
        return PreviewSubmissionResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Preview request ID space is exhausted.")});
    }

    const types::RequestId requestId = m_nextRequestId++;
    LOG_DEBUG("preview",
              "Submitting preview request {}: sequence {}, revision {}, target {}x{}, progression {}, mode {}",
              requestId,
              request.previewSequence,
              request.photo.developRevision,
              request.targetSize.width(),
              request.targetSize.height(),
              progressionName(request.progression),
              renderModeName(request.renderMode));
    const types::CancellationSource cancellationSource;
    PreviewWorkItem workItem{
        requestId,
        std::move(request),
        cancellationSource.token(),
        std::chrono::steady_clock::now(),
        0,
    };
    m_activeRequests.insert(requestId, cancellationSource);
    PreviewPipelineWorker* const worker = m_worker;
    const bool queued = QMetaObject::invokeMethod(
        worker,
        [worker, workItem = std::move(workItem)]() mutable { worker->process(std::move(workItem)); },
        Qt::QueuedConnection);

    if (!queued)
    {
        m_activeRequests.remove(requestId);
        return PreviewSubmissionResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Unable to queue preview request.")});
    }

    return PreviewSubmissionResult::success(requestId);
}

// 목적: 지정 request의 향후 frame publish를 취소하고 terminal cancellation 전달
// 입력: requestId: 취소할 request 식별자
// 출력: active request를 취소했으면 true
bool PreviewOrchestrator::cancelPreview(types::RequestId requestId)
{
    const auto request = m_activeRequests.find(requestId);

    if (request == m_activeRequests.end())
    {
        return false;
    }

    request.value().requestCancellation();
    m_activeRequests.erase(request);
    LOG_DEBUG("preview", "Cancelled preview request {}", requestId);
    emit previewCancelled(requestId);
    return true;
}

// 목적: worker frame이 아직 active이고 취소되지 않은 request인지 확인 후 전달
// 입력: result: worker가 생성한 preview 결과
// 출력: previewUpdated signal 발생 가능
void PreviewOrchestrator::handleFrameReady(const PreviewResult& result)
{
    const auto request = m_activeRequests.constFind(result.requestId);

    if (request == m_activeRequests.cend() || request.value().token().isCancellationRequested())
    {
        return;
    }

    LOG_DEBUG("preview",
              "Preview frame {}: sequence {}, tier {}, mode {}, cache hit {}, queue {} ms, cache read {} ms, "
              "decode {} ms, cache write {} ms, develop {} ms, analysis {} ms, total {} ms",
              result.requestId,
              result.previewSequence,
              tierName(result.tier),
              renderModeName(result.renderMode),
              result.stats.sourceCacheHit,
              result.stats.queueWaitMs,
              result.stats.cacheReadMs,
              result.stats.decodeMs,
              result.stats.cacheWriteMs,
              result.stats.developMs,
              result.stats.analysisMs,
              result.stats.totalMs);
    emit previewUpdated(result);
}

// 목적: worker warning이 아직 active request에 속하면 전달
// 입력: issue: worker가 생성한 warning
// 출력: previewWarning signal 발생 가능
void PreviewOrchestrator::handleWorkWarning(const PreviewIssue& issue)
{
    const auto request = m_activeRequests.constFind(issue.requestId);

    if (request == m_activeRequests.cend() || request.value().token().isCancellationRequested())
    {
        return;
    }

    LOG_WARN("preview", "Preview pipeline warning: {}", issue.error.message.toStdString());
    emit previewWarning(issue);
}

// 목적: active request를 완료 상태로 전환하고 terminal event 전달
// 입력: requestId: worker가 완료한 request 식별자
// 출력: previewCompleted signal 발생 가능
void PreviewOrchestrator::handleWorkCompleted(types::RequestId requestId)
{
    const auto request = m_activeRequests.find(requestId);

    if (request == m_activeRequests.end() || request.value().token().isCancellationRequested())
    {
        return;
    }

    m_activeRequests.erase(request);
    emit previewCompleted(requestId);
}

// 목적: active request를 실패 상태로 전환하고 terminal event 전달
// 입력: issue: worker가 생성한 terminal failure
// 출력: previewFailed signal 발생 가능
void PreviewOrchestrator::handleWorkFailed(const PreviewIssue& issue)
{
    const auto request = m_activeRequests.find(issue.requestId);

    if (request == m_activeRequests.end() || request.value().token().isCancellationRequested())
    {
        return;
    }

    m_activeRequests.erase(request);
    LOG_WARN("preview", "Preview pipeline failed: {}", issue.error.message.toStdString());
    emit previewFailed(issue);
}

}  // namespace flexraw::core::orchestration

#include "preview_pipeline_worker.h"

#include <stdexcept>
#include <utility>

namespace flexraw::core::orchestration
{
namespace
{

using Clock = std::chrono::steady_clock;

// 목적: steady clock 구간을 millisecond 단위 timing 값으로 변환
// 입력: startedAt: 측정 시작 시각
// 출력: 시작 이후 경과 millisecond
[[nodiscard]] qint64 elapsedMilliseconds(Clock::time_point startedAt)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt).count();
}

}  // namespace

// 목적: 전용 worker thread에서 사용할 synchronous pipeline 소유권 인수
// 입력: pipeline: request 처리에 사용할 non-null pipeline, parent: Qt 부모 object
// 출력: 초기화된 worker 객체
PreviewPipelineWorker::PreviewPipelineWorker(std::unique_ptr<IPreviewPipeline> pipeline, QObject* parent)
    : QObject(parent), m_pipeline(std::move(pipeline))
{
    if (m_pipeline == nullptr)
    {
        throw std::invalid_argument("Preview pipeline must not be null.");
    }
}

// 목적: 한 preview request의 progressive tier pipeline 실행
// 입력: workItem: request identity, payload와 cancellation token
// 출력: frame, warning과 terminal signal 발생 가능
void PreviewPipelineWorker::process(PreviewWorkItem workItem)
{
    try
    {
        workItem.queueWaitMs = elapsedMilliseconds(workItem.acceptedAt);

        if (isCancelled(workItem))
        {
            return;
        }

        bool hasUsableFrame = false;
        const bool renderThumbnail = workItem.request.source.kind != types::SupportedFileKind::Raw ||
                                     workItem.request.progression == PreviewProgression::Progressive;

        if (renderThumbnail)
        {
            PreviewPipelineResult thumbnail =
                m_pipeline->render(workItem.request, PreviewTier::Thumbnail, workItem.cancellationToken);

            if (isCancelled(workItem))
            {
                return;
            }

            if (thumbnail.hasValue())
            {
                publishFrame(workItem, PreviewTier::Thumbnail, std::move(thumbnail.value()));
                hasUsableFrame = true;
            }
            else if (workItem.request.source.kind == types::SupportedFileKind::Raw)
            {
                emit workWarning(makeIssue(workItem, thumbnail.error()));
            }
            else
            {
                emit workFailed(makeIssue(workItem, thumbnail.error()));
                return;
            }
        }

        if (workItem.request.source.kind == types::SupportedFileKind::Raw)
        {
            PreviewPipelineResult standard =
                m_pipeline->render(workItem.request, PreviewTier::Standard, workItem.cancellationToken);

            if (isCancelled(workItem))
            {
                return;
            }

            if (standard.hasValue())
            {
                publishFrame(workItem, PreviewTier::Standard, std::move(standard.value()));
                hasUsableFrame = true;
            }
            else if (hasUsableFrame)
            {
                emit workWarning(makeIssue(workItem, standard.error()));
            }
            else
            {
                emit workFailed(makeIssue(workItem, standard.error()));
                return;
            }
        }

        if (!isCancelled(workItem) && hasUsableFrame)
        {
            emit workCompleted(workItem.requestId);
        }
    }
    catch (const std::exception& exception)
    {
        if (!isCancelled(workItem))
        {
            emit workFailed(makeIssue(workItem, {types::ErrorCode::Unknown, QString::fromUtf8(exception.what())}));
        }
    }
    catch (...)
    {
        if (!isCancelled(workItem))
        {
            emit workFailed(makeIssue(
                workItem, {types::ErrorCode::Unknown, QStringLiteral("Unexpected preview pipeline failure.")}));
        }
    }
}

// 목적: request cancellation token 상태 확인
// 입력: workItem: 확인할 request work item
// 출력: 취소됐으면 true
bool PreviewPipelineWorker::isCancelled(const PreviewWorkItem& workItem) noexcept
{
    return workItem.cancellationToken.isCancellationRequested();
}

// 목적: pipeline frame에 request identity와 end-to-end timing을 결합해 publish
// 입력: workItem: request context, tier: rendered 품질 단계, frame: pipeline 결과
// 출력: frameReady signal 발생
void PreviewPipelineWorker::publishFrame(PreviewWorkItem& workItem, PreviewTier tier, PreviewPipelineFrame frame)
{
    frame.stats.queueWaitMs = workItem.queueWaitMs;
    frame.stats.totalMs = elapsedMilliseconds(workItem.acceptedAt);

    emit frameReady({
        workItem.requestId,
        workItem.request.photo,
        workItem.request.source.path,
        workItem.request.previewSequence,
        tier,
        workItem.request.renderMode,
        std::move(frame.image),
        std::move(frame.histogram),
        std::move(frame.clipping),
        frame.stats,
    });
}

// 목적: request context와 오류를 async issue contract로 조립
// 입력: workItem: request context, error: 전달할 구조화된 오류
// 출력: PreviewIssue 값
PreviewIssue PreviewPipelineWorker::makeIssue(const PreviewWorkItem& workItem, types::CoreError error)
{
    return PreviewIssue{
        workItem.requestId,
        workItem.request.photo,
        workItem.request.source.path,
        workItem.request.previewSequence,
        std::move(error),
    };
}

}  // namespace flexraw::core::orchestration

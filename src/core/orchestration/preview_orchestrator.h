#pragma once

#include <memory>

#include <QHash>
#include <QObject>
#include <QThread>

#include "preview_contracts.h"

namespace flexraw::core::orchestration
{

class IPreviewPipeline;
class PreviewPipelineWorker;

class PreviewOrchestrator final : public QObject
{
    Q_OBJECT

public:
    // 목적: application-scoped preview worker와 injected pipeline 조립
    // 입력: pipeline: worker thread에서 소유할 synchronous pipeline, parent: Qt 부모 object
    // 출력: 실행 가능한 Orchestrator 객체
    explicit PreviewOrchestrator(std::unique_ptr<IPreviewPipeline> pipeline, QObject* parent = nullptr);

    // 목적: 새 request 수락을 중단하고 active 작업 취소 후 worker 종료 대기
    // 입력: 없음
    // 출력: 없음
    ~PreviewOrchestrator() override;

    // 목적: preview request를 검증하고 고유 ID와 cancellation state를 부여해 background queue에 제출
    // 입력: request: photo identity, source, params와 target을 포함한 immutable 요청
    // 출력: 수락된 RequestId 또는 즉시 validation 오류
    [[nodiscard]] PreviewSubmissionResult submitPreview(PreviewRequest request);

    // 목적: 지정 request의 향후 frame publish를 취소하고 terminal cancellation 전달
    // 입력: requestId: 취소할 request 식별자
    // 출력: active request를 취소했으면 true
    bool cancelPreview(types::RequestId requestId);

signals:
    // 목적: progressive preview tier 결과를 UI adapter에 전달
    // 입력: result: request identity, rendered frame과 분석 결과
    // 출력: 없음
    void previewUpdated(const PreviewResult& result);

    // 목적: usable frame을 유지할 수 있는 비치명적 pipeline 오류 전달
    // 입력: issue: request identity와 technical error
    // 출력: 없음
    void previewWarning(const PreviewIssue& issue);

    // 목적: accepted request가 usable preview를 전달하고 정상 종료됐음을 통지
    // 입력: requestId: 완료된 request 식별자
    // 출력: 없음
    void previewCompleted(types::RequestId requestId);

    // 목적: accepted request의 terminal failure 전달
    // 입력: issue: request identity와 terminal error
    // 출력: 없음
    void previewFailed(const PreviewIssue& issue);

    // 목적: accepted request의 terminal cancellation 전달
    // 입력: requestId: 취소된 request 식별자
    // 출력: 없음
    void previewCancelled(types::RequestId requestId);

private:
    // 목적: worker frame이 아직 active이고 취소되지 않은 request인지 확인 후 전달
    // 입력: result: worker가 생성한 preview 결과
    // 출력: previewUpdated signal 발생 가능
    void handleFrameReady(const PreviewResult& result);

    // 목적: worker warning이 아직 active request에 속하면 전달
    // 입력: issue: worker가 생성한 warning
    // 출력: previewWarning signal 발생 가능
    void handleWorkWarning(const PreviewIssue& issue);

    // 목적: active request를 완료 상태로 전환하고 terminal event 전달
    // 입력: requestId: worker가 완료한 request 식별자
    // 출력: previewCompleted signal 발생 가능
    void handleWorkCompleted(types::RequestId requestId);

    // 목적: active request를 실패 상태로 전환하고 terminal event 전달
    // 입력: issue: worker가 생성한 terminal failure
    // 출력: previewFailed signal 발생 가능
    void handleWorkFailed(const PreviewIssue& issue);

    QThread m_workerThread;
    PreviewPipelineWorker* m_worker{nullptr};
    QHash<types::RequestId, types::CancellationSource> m_activeRequests;
    types::RequestId m_nextRequestId{1};
    bool m_acceptingRequests{true};
};

}  // namespace flexraw::core::orchestration

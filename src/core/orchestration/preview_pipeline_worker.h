#pragma once

#include <chrono>
#include <memory>

#include <QObject>

#include "preview_pipeline.h"

namespace flexraw::core::orchestration
{

struct PreviewWorkItem
{
    types::RequestId requestId{0};
    PreviewRequest request;
    types::CancellationToken cancellationToken;
    std::chrono::steady_clock::time_point acceptedAt;
    qint64 queueWaitMs{0};
};

class PreviewPipelineWorker final : public QObject
{
    Q_OBJECT

public:
    // 목적: 전용 worker thread에서 사용할 synchronous pipeline 소유권 인수
    // 입력: pipeline: request 처리에 사용할 non-null pipeline, parent: Qt 부모 object
    // 출력: 초기화된 worker 객체
    explicit PreviewPipelineWorker(std::unique_ptr<IPreviewPipeline> pipeline, QObject* parent = nullptr);

    // 목적: 한 preview request의 progressive tier pipeline 실행
    // 입력: workItem: request identity, payload와 cancellation token
    // 출력: frame, warning과 terminal signal 발생 가능
    void process(PreviewWorkItem workItem);

signals:
    // 목적: 취소되지 않은 progressive preview frame 전달
    // 입력: result: request identity와 rendered frame
    // 출력: 없음
    void frameReady(const PreviewResult& result);

    // 목적: usable frame이 있거나 다음 tier로 복구 가능한 비치명적 오류 전달
    // 입력: issue: request identity와 technical error
    // 출력: 없음
    void workWarning(const PreviewIssue& issue);

    // 목적: request가 하나 이상의 usable frame을 생성하고 종료됐음을 전달
    // 입력: requestId: 완료된 request 식별자
    // 출력: 없음
    void workCompleted(types::RequestId requestId);

    // 목적: usable 결과 없이 request pipeline이 실패했음을 전달
    // 입력: issue: request identity와 terminal error
    // 출력: 없음
    void workFailed(const PreviewIssue& issue);

private:
    // 목적: request cancellation token 상태 확인
    // 입력: workItem: 확인할 request work item
    // 출력: 취소됐으면 true
    [[nodiscard]] static bool isCancelled(const PreviewWorkItem& workItem) noexcept;

    // 목적: pipeline frame에 request identity와 end-to-end timing을 결합해 publish
    // 입력: workItem: request context, tier: rendered 품질 단계, frame: pipeline 결과
    // 출력: frameReady signal 발생
    void publishFrame(PreviewWorkItem& workItem, PreviewTier tier, PreviewPipelineFrame frame);

    // 목적: request context와 오류를 async issue contract로 조립
    // 입력: workItem: request context, error: 전달할 구조화된 오류
    // 출력: PreviewIssue 값
    [[nodiscard]] static PreviewIssue makeIssue(const PreviewWorkItem& workItem, types::CoreError error);

    std::unique_ptr<IPreviewPipeline> m_pipeline;
};

}  // namespace flexraw::core::orchestration

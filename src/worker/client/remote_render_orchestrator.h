#pragma once

#include <cstdint>
#include <memory>

#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QThreadPool>

#include "remote_render_executor.h"
#include "remote_render_preflight.h"

namespace flexraw::worker::client
{

struct RemoteRenderOperation
{
    RemoteWorkerProfile profile;
    core::render::ResolvedRenderRequest request;
};

struct RemoteRenderCompletion
{
    core::types::RequestId requestId{0};
    core::render::ResolvedRenderResult result;
};

struct RemoteRenderIssue
{
    core::types::RequestId requestId{0};
    RemoteRenderError error;
};

struct RemoteRenderSubmissionError
{
    RemoteRenderPreflightErrorCode code{RemoteRenderPreflightErrorCode::PathMappingFailed};
    core::types::CoreError cause;
};

using RemoteRenderSubmissionResult = core::types::Result<core::types::RequestId, RemoteRenderSubmissionError>;

class RemoteRenderOrchestrator final : public QObject
{
    Q_OBJECT

public:
    // 목적: application-scoped remote render pool과 injected synchronous executor 조립
    // 입력: executor: TCP 실행 implementation, maximumConcurrentJobs: bounded client 동시 실행 수, parent: Qt 부모
    // 출력: async submit/cancel이 가능한 remote client orchestrator
    explicit RemoteRenderOrchestrator(std::unique_ptr<IRemoteRenderExecutor> executor,
                                      int maximumConcurrentJobs = 1,
                                      QObject* parent = nullptr);

    // 목적: 신규 submit을 차단하고 active remote operation을 취소한 뒤 client pool 종료 대기
    // 입력: 없음
    // 출력: 없음
    ~RemoteRenderOrchestrator() override;

    // 목적: shared storage marker와 profile identity를 확인한 뒤 background remote execution으로 제출
    // 입력: operation: manual Worker profile과 Desktop absolute resolved render request
    // 출력: 수락된 RequestId 또는 network 시작 전 storage/mapping/lifecycle 오류
    [[nodiscard]] RemoteRenderSubmissionResult submit(RemoteRenderOperation operation);

    // 목적: active remote operation의 socket cancellation을 요청하고 terminal event 발행
    // 입력: requestId: 취소할 client operation identity
    // 출력: active operation을 취소했으면 true
    bool cancel(core::types::RequestId requestId);

signals:
    // 목적: remote artifact path를 Desktop local path로 복원한 성공 결과 전달
    // 입력: completion: client request identity와 artifact/stats
    // 출력: 없음
    void remoteRenderCompleted(const RemoteRenderCompletion& completion);

    // 목적: accepted remote operation의 transport/worker terminal failure 전달
    // 입력: issue: client request identity와 typed remote 오류
    // 출력: 없음
    void remoteRenderFailed(const RemoteRenderIssue& issue);

    // 목적: accepted remote operation의 terminal cancellation 전달
    // 입력: requestId: 취소된 client operation identity
    // 출력: 없음
    void remoteRenderCancelled(core::types::RequestId requestId);

private:
    struct ActiveRemoteJob
    {
        core::types::CancellationSource cancellationSource;
        QString localOutputPath;
    };

    // 목적: worker future 결과를 active job에서 제거하고 단일 terminal event로 변환
    // 입력: requestId: client operation identity, result: synchronous executor 결과
    // 출력: completion/failure/cancellation signal 하나 발생 가능
    void handleFinished(core::types::RequestId requestId, RemoteRenderResult result);

    std::unique_ptr<IRemoteRenderExecutor> m_executor;
    QThreadPool m_jobPool;
    QHash<core::types::RequestId, ActiveRemoteJob> m_activeJobs;
    core::types::RequestId m_nextRequestId{1};
    bool m_acceptingRequests{true};
};

}  // namespace flexraw::worker::client

Q_DECLARE_METATYPE(flexraw::worker::client::RemoteRenderCompletion)
Q_DECLARE_METATYPE(flexraw::worker::client::RemoteRenderIssue)

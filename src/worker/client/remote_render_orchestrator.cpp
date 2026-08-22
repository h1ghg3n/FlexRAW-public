#include "remote_render_orchestrator.h"

#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#include <QMetaObject>

namespace flexraw::worker::client
{
namespace
{

// 목적: preflight/lifecycle 분류와 공통 CoreError를 submission 오류로 구성
// 입력: code/coreCode/message: submission 원인과 adapter-facing 진단
// 출력: 구성된 RemoteRenderSubmissionError
[[nodiscard]] RemoteRenderSubmissionError makeSubmissionError(const RemoteRenderPreflightErrorCode code,
                                                               const core::types::ErrorCode coreCode,
                                                               QString message)
{
    return {code, {coreCode, std::move(message)}};
}

// 목적: 공통 preflight 오류를 기존 submission error contract로 감쌈
// 입력: error: marker/profile/path typed 오류
// 출력: preflight 분류와 CoreError를 보존한 submission 오류
[[nodiscard]] RemoteRenderSubmissionError makeSubmissionError(const RemoteRenderPreflightError& error)
{
    return {error.code, error.cause};
}

// 목적: executor 경계 밖으로 전달된 C++ exception을 typed remote failure로 변환
// 입력: message: exception 상세 또는 fallback 설명
// 출력: RenderFailed/Unknown terminal result
[[nodiscard]] RemoteRenderResult makeUnhandledFailure(QString message)
{
    return RemoteRenderResult::failure(
        {RemoteRenderErrorCode::RenderFailed, {core::types::ErrorCode::Unknown, std::move(message)}, {}, std::nullopt});
}

}  // namespace

// 목적: application-scoped remote render pool과 injected synchronous executor 조립
// 입력: executor: TCP 실행 implementation, maximumConcurrentJobs: bounded client 동시 실행 수, parent: Qt 부모
// 출력: async submit/cancel이 가능한 remote client orchestrator
RemoteRenderOrchestrator::RemoteRenderOrchestrator(std::unique_ptr<IRemoteRenderExecutor> executor,
                                                   const int maximumConcurrentJobs,
                                                   QObject* parent)
    : QObject(parent), m_executor(std::move(executor))
{
    if (m_executor == nullptr)
    {
        throw std::invalid_argument("Remote render executor must not be null.");
    }
    if (maximumConcurrentJobs <= 0)
    {
        throw std::invalid_argument("Remote render client concurrency must be positive.");
    }

    qRegisterMetaType<RemoteRenderCompletion>();
    qRegisterMetaType<RemoteRenderIssue>();
    m_jobPool.setObjectName(QStringLiteral("RemoteRenderClientPool"));
    m_jobPool.setMaxThreadCount(maximumConcurrentJobs);
}

// 목적: 신규 submit을 차단하고 active remote operation을 취소한 뒤 client pool 종료 대기
// 입력: 없음
// 출력: 없음
RemoteRenderOrchestrator::~RemoteRenderOrchestrator()
{
    m_acceptingRequests = false;
    for (const ActiveRemoteJob& job : std::as_const(m_activeJobs))
    {
        job.cancellationSource.requestCancellation();
    }
    m_jobPool.waitForDone();
    m_activeJobs.clear();
}

// 목적: shared storage marker와 profile identity를 확인한 뒤 background remote execution으로 제출
// 입력: operation: manual Worker profile과 Desktop absolute resolved render request
// 출력: 수락된 RequestId 또는 network 시작 전 storage/mapping/lifecycle 오류
RemoteRenderSubmissionResult RemoteRenderOrchestrator::submit(RemoteRenderOperation operation)
{
    if (!m_acceptingRequests)
    {
        return RemoteRenderSubmissionResult::failure(
            makeSubmissionError(RemoteRenderPreflightErrorCode::ShuttingDown,
                                core::types::ErrorCode::Unknown,
                                QStringLiteral("Remote render orchestrator is shutting down.")));
    }
    if (m_nextRequestId == std::numeric_limits<core::types::RequestId>::max())
    {
        return RemoteRenderSubmissionResult::failure(
            makeSubmissionError(RemoteRenderPreflightErrorCode::RequestIdExhausted,
                                core::types::ErrorCode::Unknown,
                                QStringLiteral("Remote render request ID space is exhausted.")));
    }

    RemoteRenderPreflightResult mapped = prepareRemoteRenderRequest(operation.profile, operation.request);
    if (mapped.hasError())
    {
        return RemoteRenderSubmissionResult::failure(makeSubmissionError(mapped.error()));
    }

    const core::types::RequestId requestId = m_nextRequestId++;
    const core::types::CancellationSource cancellationSource;
    m_activeJobs.insert(requestId, {cancellationSource, operation.request.outputPath});

    const core::types::CancellationToken cancellationToken = cancellationSource.token();
    IRemoteRenderExecutor* const executor = m_executor.get();
    const RemoteRenderEndpoint endpoint = operation.profile.endpoint;
    m_jobPool.start(
        [this, executor, endpoint, requestId, request = std::move(mapped.value()), cancellationToken]() mutable {
            RemoteRenderResult result = RemoteRenderResult::failure({});
            try
            {
                result = executor->execute(endpoint, request, cancellationToken);
            }
            catch (const std::exception& error)
            {
                result =
                    makeUnhandledFailure(QStringLiteral("Unhandled remote render exception: %1").arg(error.what()));
            }
            catch (...)
            {
                result = makeUnhandledFailure(QStringLiteral("Unhandled non-standard remote render exception."));
            }

            (void)QMetaObject::invokeMethod(
                this,
                [this, requestId, result = std::move(result)]() mutable {
                    handleFinished(requestId, std::move(result));
                },
                Qt::QueuedConnection);
        });
    return RemoteRenderSubmissionResult::success(requestId);
}

// 목적: active remote operation의 socket cancellation을 요청하고 terminal event 발행
// 입력: requestId: 취소할 client operation identity
// 출력: active operation을 취소했으면 true
bool RemoteRenderOrchestrator::cancel(const core::types::RequestId requestId)
{
    const auto job = m_activeJobs.find(requestId);
    if (job == m_activeJobs.end())
    {
        return false;
    }

    job.value().cancellationSource.requestCancellation();
    m_activeJobs.erase(job);
    emit remoteRenderCancelled(requestId);
    return true;
}

// 목적: worker future 결과를 active job에서 제거하고 단일 terminal event로 변환
// 입력: requestId: client operation identity, result: synchronous executor 결과
// 출력: completion/failure/cancellation signal 하나 발생 가능
void RemoteRenderOrchestrator::handleFinished(const core::types::RequestId requestId, RemoteRenderResult result)
{
    const auto job = m_activeJobs.find(requestId);
    if (job == m_activeJobs.end())
    {
        return;
    }

    const QString localOutputPath = job.value().localOutputPath;
    const bool cancellationRequested = job.value().cancellationSource.token().isCancellationRequested();
    m_activeJobs.erase(job);
    if (cancellationRequested || (result.hasError() && result.error().code == RemoteRenderErrorCode::Cancelled))
    {
        emit remoteRenderCancelled(requestId);
        return;
    }
    if (result.hasError())
    {
        emit remoteRenderFailed({requestId, result.error()});
        return;
    }

    result.value().artifact.outputPath = localOutputPath;
    emit remoteRenderCompleted({requestId, result.value()});
}

}  // namespace flexraw::worker::client

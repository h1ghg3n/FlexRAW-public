#include "remote_export_execution_adapter.h"

#include <stdexcept>
#include <utility>

#include <QUuid>

#include "remote_render_preflight.h"

namespace flexraw::worker::client
{
namespace
{

// 목적: Remote adapter 사전 검증 오류를 placement eligibility failure로 변환
// 입력: cause: profile/source/path 검증의 project-owned 오류
// 출력: network를 시작하지 않은 Ineligible failure
[[nodiscard]] core::orchestration::RemoteExportExecutionResult makeIneligible(core::types::CoreError cause)
{
    return core::orchestration::RemoteExportExecutionResult::failure(
        {core::orchestration::RemoteExportFailureCode::Ineligible, std::move(cause), {}, false});
}

// 목적: Worker client 오류 분류를 Export placement failure 분류로 변환
// 입력: code: RemoteRenderExecutor의 typed transport/worker 오류
// 출력: Core Orchestration이 해석하는 normalized failure code
[[nodiscard]] core::orchestration::RemoteExportFailureCode mapFailureCode(const RemoteRenderErrorCode code)
{
    using core::orchestration::RemoteExportFailureCode;
    switch (code)
    {
    case RemoteRenderErrorCode::InvalidEndpoint:
    case RemoteRenderErrorCode::InvalidRequest:
        return RemoteExportFailureCode::Ineligible;
    case RemoteRenderErrorCode::ConnectionFailed:
        return RemoteExportFailureCode::ConnectionFailed;
    case RemoteRenderErrorCode::ConnectionLost:
        return RemoteExportFailureCode::ConnectionLost;
    case RemoteRenderErrorCode::TimedOut:
        return RemoteExportFailureCode::TimedOut;
    case RemoteRenderErrorCode::ProtocolViolation:
        return RemoteExportFailureCode::ProtocolViolation;
    case RemoteRenderErrorCode::ServerBusy:
        return RemoteExportFailureCode::ServerBusy;
    case RemoteRenderErrorCode::ResourceBusy:
        return RemoteExportFailureCode::ResourceBusy;
    case RemoteRenderErrorCode::RenderFailed:
        return RemoteExportFailureCode::RenderFailed;
    case RemoteRenderErrorCode::Cancelled:
        return RemoteExportFailureCode::Cancelled;
    }
    return RemoteExportFailureCode::ProtocolViolation;
}

}  // namespace

// 목적: shared-storage preflight와 TCP executor를 하나의 Export execution port로 조립
// 입력: executor: synchronous Remote render implementation
// 출력: ExportOrchestrator에 주입 가능한 adapter
RemoteExportExecutionAdapter::RemoteExportExecutionAdapter(std::unique_ptr<IRemoteRenderExecutor> executor)
    : m_executor(std::move(executor))
{
    if (m_executor == nullptr)
    {
        throw std::invalid_argument("Remote export executor must not be null.");
    }
}

// 목적: resolved RAW export item을 preflight한 뒤 manual Worker endpoint에서 실행
// 입력: target/item/token: 실행 값, accepted: Worker ownership 확정 callback
// 출력: Desktop local output path를 보존한 item 결과 또는 normalized placement failure
core::orchestration::RemoteExportExecutionResult RemoteExportExecutionAdapter::execute(
    const core::orchestration::ExportRemoteTarget& target,
    const core::orchestration::PreparedExportItem& item,
    const core::types::CancellationToken& cancellationToken,
    const core::orchestration::RemoteExportAcceptedCallback& accepted) const
{
    if (item.preparationError.has_value())
    {
        return makeIneligible(*item.preparationError);
    }
    if (item.request.source.kind != core::types::SupportedFileKind::Raw || !item.request.developParams.has_value())
    {
        return makeIneligible({core::types::ErrorCode::UnsupportedFormat,
                               QStringLiteral("Remote export requires a resolved RAW item.")});
    }

    RemoteWorkerProfile profile;
    profile.endpoint.host = target.host;
    profile.endpoint.port = target.port;
    profile.expectedSourceStorageId = QUuid::fromString(target.expectedSourceStorageId);
    profile.expectedOutputStorageId = QUuid::fromString(target.expectedOutputStorageId);

    const core::render::ResolvedRenderRequest resolved{item.request.source.path,
                                                       item.request.outputPath,
                                                       *item.request.developParams,
                                                       item.request.options};
    RemoteRenderPreflightResult mapped = prepareRemoteRenderRequest(profile, resolved);
    if (mapped.hasError())
    {
        return makeIneligible(mapped.error().cause);
    }

    bool workerAccepted = false;
    const RemoteRenderResult rendered = m_executor->execute(
        profile.endpoint,
        mapped.value(),
        cancellationToken,
        [&workerAccepted, &accepted] {
            workerAccepted = true;
            if (accepted)
            {
                accepted();
            }
        });
    if (rendered.hasError())
    {
        return core::orchestration::RemoteExportExecutionResult::failure(
            {mapFailureCode(rendered.error().code),
             rendered.error().cause,
             rendered.error().retryAfter,
             workerAccepted});
    }

    return core::orchestration::RemoteExportExecutionResult::success(
        {item.request.source.path, item.request.outputPath, true, {}, core::orchestration::ExportItemFailureKind::None});
}

}  // namespace flexraw::worker::client

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include <QString>

#include "develop_params.h"
#include "export_options.h"
#include "render_job_identity.h"
#include "resolved_render_pipeline.h"
#include "result.h"
#include "runtime_observation_contracts.h"

namespace flexraw::worker::runtime
{

struct RenderWorkerRequest
{
    QString sourceRelativePath;
    QString outputRelativePath;
    core::types::DevelopParams developParams;
    core::export_::RasterExportOptions outputOptions;
};

struct RenderWorkerCommand
{
    RenderJobKey key;
    RenderWorkerRequest request;
};

enum class WorkerPathErrorCode
{
    InvalidRoot,
    InvalidRelativePath,
    SourceNotFound,
    SourceOutsideRoot,
    OutputParentNotFound,
    OutputOutsideRoot,
    InvalidRequestValue,
};

struct WorkerPathError
{
    WorkerPathErrorCode code{WorkerPathErrorCode::InvalidRelativePath};
    QString message;
};

struct RenderResourceBusy
{
    QString message;
    std::optional<std::chrono::milliseconds> retryAfter;
};

using RenderJobExecutionResult = core::types::Result<core::render::ResolvedRenderPipelineResult, RenderResourceBusy>;

struct RenderJobOutcome
{
    RenderJobKey key;
    QString outputRelativePath;
    RenderJobExecutionResult result;
};

using RenderJobCompletion = std::function<void(RenderJobOutcome)>;

enum class SubmitStatus
{
    Accepted,
    InvalidJobId,
    DuplicateJobId,
    QueueFull,
    ShuttingDown,
};

using RenderWorkerSubmitResult = core::types::Result<SubmitStatus, WorkerPathError>;

class IRenderWorkerRuntime
{
public:
    // 목적: Server Adapter가 runtime port를 안전하게 소멸할 수 있는 virtual 경계 제공
    // 입력: 없음
    // 출력: 없음
    virtual ~IRenderWorkerRuntime() = default;

    // 목적: root-relative render command를 기존 Runtime validation과 scheduler에 제출
    // 입력: command: session job identity와 processing 값, completion: 임의 thread에서 오는 terminal callback
    // 출력: path/value 오류 또는 기존 scheduler 접수 상태
    [[nodiscard]] virtual RenderWorkerSubmitResult submit(RenderWorkerCommand command,
                                                          RenderJobCompletion completion) = 0;

    // 목적: queued 또는 running render command에 cooperative cancellation 전달
    // 입력: key: session과 job을 결합한 Runtime identity
    // 출력: active command를 찾아 취소했으면 true
    [[nodiscard]] virtual bool cancel(RenderJobKey key) = 0;

    // 목적: Server Adapter가 현재 Runtime load와 configured limit을 read-only로 관측
    // 입력: 없음
    // 출력: scheduling 또는 admission authority로 사용하지 않는 thread-safe snapshot
    [[nodiscard]] virtual WorkerRuntimeSnapshot snapshot() const = 0;
};

}  // namespace flexraw::worker::runtime

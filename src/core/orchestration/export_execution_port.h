#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "export_pipeline.h"

namespace flexraw::core::orchestration
{

enum class RemoteExportFailureCode : std::uint8_t
{
    Ineligible,
    ServerBusy,
    ResourceBusy,
    ConnectionFailed,
    ConnectionLost,
    TimedOut,
    ProtocolViolation,
    RenderFailed,
    Cancelled,
};

struct RemoteExportFailure
{
    RemoteExportFailureCode code{RemoteExportFailureCode::ConnectionFailed};
    types::CoreError cause;
    std::optional<std::chrono::milliseconds> retryAfter;
    bool workerAccepted{false};
};

using RemoteExportExecutionResult = types::Result<ExportItemResult, RemoteExportFailure>;
using RemoteExportAcceptedCallback = std::function<void()>;

class IRemoteExportExecutionPort
{
public:
    // 목적: injected Remote execution adapter를 interface pointer로 안전하게 소멸
    // 입력: 없음
    // 출력: 없음
    virtual ~IRemoteExportExecutionPort() = default;

    // 목적: 한 resolved export item을 configured Remote target에서 동기 실행
    // 입력: target/item/token: immutable 실행 값, accepted: Worker ownership 확정 callback
    // 출력: local path로 복원된 item 결과 또는 placement용 normalized failure
    [[nodiscard]] virtual RemoteExportExecutionResult execute(
        const ExportRemoteTarget& target,
        const PreparedExportItem& item,
        const types::CancellationToken& cancellationToken,
        const RemoteExportAcceptedCallback& accepted) const = 0;
};

}  // namespace flexraw::core::orchestration

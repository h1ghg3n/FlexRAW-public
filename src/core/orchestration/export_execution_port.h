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
    // 입력: target/item/token: immutable 값, accepted: 같은 execute thread에서 0~1회 호출하고 저장하지 않는 callback
    // 출력: callback 완료와 Remote 실행이 모두 quiescent한 item 결과 또는 placement용 normalized failure
    [[nodiscard]] virtual RemoteExportExecutionResult execute(const ExportRemoteTarget& target,
                                                              const PreparedExportItem& item,
                                                              const types::CancellationToken& cancellationToken,
                                                              const RemoteExportAcceptedCallback& accepted) const = 0;
};

}  // namespace flexraw::core::orchestration

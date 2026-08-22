#pragma once

#include <memory>

#include "export_execution_port.h"
#include "remote_render_executor.h"

namespace flexraw::worker::client
{

class RemoteExportExecutionAdapter final : public core::orchestration::IRemoteExportExecutionPort
{
public:
    // 목적: shared-storage preflight와 TCP executor를 하나의 Export execution port로 조립
    // 입력: executor: synchronous Remote render implementation
    // 출력: ExportOrchestrator에 주입 가능한 adapter
    explicit RemoteExportExecutionAdapter(std::unique_ptr<IRemoteRenderExecutor> executor);

    // 목적: resolved RAW export item을 preflight한 뒤 manual Worker endpoint에서 실행
    // 입력: target/item/token: 실행 값, accepted: Worker ownership 확정 callback
    // 출력: Desktop local output path를 보존한 item 결과 또는 normalized placement failure
    [[nodiscard]] core::orchestration::RemoteExportExecutionResult execute(
        const core::orchestration::ExportRemoteTarget& target,
        const core::orchestration::PreparedExportItem& item,
        const core::types::CancellationToken& cancellationToken,
        const core::orchestration::RemoteExportAcceptedCallback& accepted) const override;

private:
    std::unique_ptr<IRemoteRenderExecutor> m_executor;
};

}  // namespace flexraw::worker::client

#pragma once

#include "render_job_runner.h"
#include "render_resource_admission.h"

namespace flexraw::worker::admission
{

class AdmissionRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: 기존 synchronous render runner 앞에 resource lease lifecycle을 결합
    // 입력: delegate: 실제 processing runner, admission: local 또는 Router admission, configuration: declared claim
    // 출력: processing dependency보다 먼저 파괴되어야 하는 admission-aware runner
    AdmissionRenderJobRunner(const runtime::IRenderJobRunner& delegate,
                             IRenderResourceAdmission& admission,
                             ResourceAdmissionConfiguration configuration);

    // 목적: render 실행 전 lease acquire, 실행 중 renew, 종료 후 release 수행
    // 입력: request: resolved processing request, cancellationToken: scheduler cancellation state
    // 출력: admission 실패 또는 renew loss면 CoreError failure, 아니면 delegate terminal result
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override;

private:
    const runtime::IRenderJobRunner& m_delegate;
    IRenderResourceAdmission& m_admission;
    ResourceAdmissionConfiguration m_configuration;
};

}  // namespace flexraw::worker::admission

#pragma once

#include "resolved_render_pipeline.h"

namespace flexraw::core::render
{

class LocalRenderExecutor final
{
public:
    // 목적: caller가 소유한 resolved render pipeline을 local 실행 경로로 주입
    // 입력: pipeline: executor보다 오래 살아야 하는 synchronous pipeline
    // 출력: local render executor
    explicit LocalRenderExecutor(const IResolvedRenderPipeline& pipeline) noexcept;

    // 목적: 별도 thread를 생성하지 않고 caller context에서 resolved render 실행
    // 입력: request: local path로 resolved된 render 요청, cancellationToken: cooperative 중단 상태
    // 출력: pipeline이 반환한 artifact/stats 또는 failure/stats
    [[nodiscard]] ResolvedRenderPipelineResult execute(const ResolvedRenderRequest& request,
                                                       const types::CancellationToken& cancellationToken) const;

private:
    const IResolvedRenderPipeline& m_pipeline;
};

}  // namespace flexraw::core::render

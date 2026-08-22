#include "render_job_runner.h"

namespace flexraw::worker::runtime
{

// 목적: scheduler와 synchronous render pipeline 사이의 실행 adapter 생성
// 입력: pipeline: runner보다 오래 살아야 하는 processing pipeline
// 출력: pipeline-backed job runner
PipelineRenderJobRunner::PipelineRenderJobRunner(const core::render::IResolvedRenderPipeline& pipeline) noexcept
    : m_pipeline(pipeline)
{}

// 목적: resolved request를 주입된 pipeline에서 동기 실행
// 입력: request: resolved processing 값, cancellationToken: cooperative 중단 상태
// 출력: pipeline artifact/stats 또는 failure/stats
RenderJobExecutionResult PipelineRenderJobRunner::execute(const core::render::ResolvedRenderRequest& request,
                                                          const core::types::CancellationToken& cancellationToken) const
{
    return RenderJobExecutionResult::success(m_pipeline.execute(request, cancellationToken));
}

}  // namespace flexraw::worker::runtime

#pragma once

#include "operation_types.h"
#include "render_worker_runtime_port.h"

namespace flexraw::worker::runtime
{

class IRenderJobRunner
{
public:
    // 목적: scheduler가 polymorphic job runner를 안전하게 소멸할 수 있는 virtual 경계 제공
    // 입력: 없음
    // 출력: 없음
    virtual ~IRenderJobRunner() = default;

    // 목적: 이미 local path로 해석된 single render job 동기 실행
    // 입력: request: resolved processing 값, cancellationToken: cooperative 중단 상태
    // 출력: artifact/stats 또는 failure/stats
    [[nodiscard]] virtual RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const = 0;
};

class PipelineRenderJobRunner final : public IRenderJobRunner
{
public:
    // 목적: scheduler와 synchronous render pipeline 사이의 실행 adapter 생성
    // 입력: pipeline: runner보다 오래 살아야 하는 processing pipeline
    // 출력: pipeline-backed job runner
    explicit PipelineRenderJobRunner(const core::render::IResolvedRenderPipeline& pipeline) noexcept;

    // 목적: resolved request를 주입된 pipeline에서 동기 실행
    // 입력: request: resolved processing 값, cancellationToken: cooperative 중단 상태
    // 출력: pipeline artifact/stats 또는 failure/stats
    [[nodiscard]] RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override;

private:
    const core::render::IResolvedRenderPipeline& m_pipeline;
};

}  // namespace flexraw::worker::runtime

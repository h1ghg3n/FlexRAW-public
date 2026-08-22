#pragma once

#include "operation_types.h"
#include "render_contracts.h"

namespace flexraw::core::render
{

class IResolvedRenderPipeline
{
public:
    virtual ~IResolvedRenderPipeline() = default;

    // 목적: resolved single-RAW request를 synchronous processing path로 실행
    // 입력: request: local 경로·parameter·output option, cancellationToken: cooperative 중단 상태
    // 출력: output artifact와 stage timing 또는 timing을 포함한 구조화된 실패
    [[nodiscard]] virtual ResolvedRenderPipelineResult execute(
        const ResolvedRenderRequest& request, const types::CancellationToken& cancellationToken) const = 0;
};

class ResolvedRenderPipeline final : public IResolvedRenderPipeline
{
public:
    // 목적: RAW decode, source conversion, develop, raster output을 순차적으로 실행
    // 입력: request: resolved render 값, cancellationToken: stage 사이에 확인할 중단 상태
    // 출력: atomic output artifact와 RenderStats 또는 stage failure
    [[nodiscard]] ResolvedRenderPipelineResult execute(
        const ResolvedRenderRequest& request, const types::CancellationToken& cancellationToken) const override;
};

}  // namespace flexraw::core::render

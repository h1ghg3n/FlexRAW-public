#pragma once

#include <memory>

#include <QString>

#include "preview_contracts.h"

namespace flexraw::core::orchestration
{

struct PreviewPipelineFrame
{
    QImage image;
    develop::ImageHistogram histogram;
    develop::ClippingSummary clipping;
    PreviewOperationStats stats;
};

using PreviewPipelineResult = types::Result<PreviewPipelineFrame, types::CoreError>;

class IPreviewPipeline
{
public:
    // 목적: preview pipeline implementation의 다형적 정리를 보장
    // 입력: 없음
    // 출력: 없음
    virtual ~IPreviewPipeline() = default;

    // 목적: 요청된 tier의 source decode, develop과 분석을 동기 실행
    // 입력: request: immutable preview 요청, tier: 생성할 preview 품질 단계, cancellationToken: 중단 상태
    // 출력: 완성된 frame 또는 구조화된 오류
    [[nodiscard]] virtual PreviewPipelineResult render(const PreviewRequest& request,
                                                       PreviewTier tier,
                                                       const types::CancellationToken& cancellationToken) = 0;
};

class FilePreviewPipeline final : public IPreviewPipeline
{
public:
    // 목적: file-backed preview cache를 사용하는 production pipeline 초기화
    // 입력: cacheRoot: preview cache root directory
    // 출력: 초기화된 pipeline 객체
    explicit FilePreviewPipeline(QString cacheRoot);

    // 목적: pipeline implementation resource 정리
    // 입력: 없음
    // 출력: 없음
    ~FilePreviewPipeline() override;

    // 목적: cache와 file decoder를 통해 요청 tier를 현상하고 분석
    // 입력: request: immutable preview 요청, tier: 생성할 preview 품질 단계, cancellationToken: 중단 상태
    // 출력: 완성된 frame 또는 구조화된 오류
    [[nodiscard]] PreviewPipelineResult render(const PreviewRequest& request,
                                               PreviewTier tier,
                                               const types::CancellationToken& cancellationToken) override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace flexraw::core::orchestration

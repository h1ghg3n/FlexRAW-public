#pragma once

#include <QImage>
#include <QSize>

#include "error.h"
#include "file_types.h"
#include "operation_types.h"
#include "result.h"

namespace flexraw::core::orchestration
{

using CatalogThumbnailPipelineResult = types::Result<QImage, types::CoreError>;

class ICatalogThumbnailPipeline
{
public:
    virtual ~ICatalogThumbnailPipeline() = default;

    // 목적: file source에서 목록용 thumbnail frame 생성
    // 입력: source: immutable file 정보, targetSize: 최대 크기, cancellationToken: stale 작업 중단 상태
    // 출력: target 이하 QImage 또는 구조화된 오류
    [[nodiscard]] virtual CatalogThumbnailPipelineResult load(const types::FileDescriptor& source,
                                                              const QSize& targetSize,
                                                              const types::CancellationToken& cancellationToken) = 0;
};

class FileCatalogThumbnailPipeline final : public ICatalogThumbnailPipeline
{
public:
    // 목적: RAW embedded thumbnail 또는 raster decode로 목록용 frame 생성
    // 입력: source: immutable file 정보, targetSize: 최대 크기, cancellationToken: stale 작업 중단 상태
    // 출력: disk cache를 추가하지 않은 target 이하 QImage 또는 구조화된 오류
    [[nodiscard]] CatalogThumbnailPipelineResult load(const types::FileDescriptor& source,
                                                      const QSize& targetSize,
                                                      const types::CancellationToken& cancellationToken) override;
};

}  // namespace flexraw::core::orchestration

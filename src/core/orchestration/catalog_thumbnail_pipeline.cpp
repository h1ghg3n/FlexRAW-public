#include "catalog_thumbnail_pipeline.h"

#include "thumbnail_preview.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: cooperative cancellation을 thumbnail pipeline 결과로 변환
// 입력: 없음
// 출력: Cancelled CoreError를 보유한 실패 결과
[[nodiscard]] CatalogThumbnailPipelineResult makeCancelledResult()
{
    return CatalogThumbnailPipelineResult::failure(
        {types::ErrorCode::Cancelled, QStringLiteral("Catalog thumbnail request was cancelled.")});
}

}  // namespace

// 목적: RAW embedded thumbnail 또는 raster decode로 목록용 frame 생성
// 입력: source: immutable file 정보, targetSize: 최대 크기, cancellationToken: stale 작업 중단 상태
// 출력: disk cache를 추가하지 않은 target 이하 QImage 또는 구조화된 오류
CatalogThumbnailPipelineResult FileCatalogThumbnailPipeline::load(const types::FileDescriptor& source,
                                                                  const QSize& targetSize,
                                                                  const types::CancellationToken& cancellationToken)
{
    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    CatalogThumbnailPipelineResult result = preview::loadFilePreview(source, targetSize);
    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }
    return result;
}

}  // namespace flexraw::core::orchestration

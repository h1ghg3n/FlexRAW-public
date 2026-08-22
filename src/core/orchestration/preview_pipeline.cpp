#include "preview_pipeline.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

#include <QDir>
#include <QFileInfo>

#include "develop.h"
#include "log.h"
#include "preview_cache.h"
#include "thumbnail_preview.h"

namespace flexraw::core::orchestration
{
namespace
{

using Clock = std::chrono::steady_clock;
constexpr int InteractivePreviewScaleDivisor = 2;

// 목적: steady clock 구간을 millisecond 단위 timing 값으로 변환
// 입력: startedAt: 측정 시작 시각
// 출력: 시작 이후 경과 millisecond
[[nodiscard]] qint64 elapsedMilliseconds(Clock::time_point startedAt)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt).count();
}

// 목적: orchestration preview tier를 file cache tier로 변환
// 입력: tier: orchestration contract의 품질 단계
// 출력: PreviewCache가 사용하는 품질 단계
[[nodiscard]] preview::PreviewCacheTier toCacheTier(PreviewTier tier)
{
    return tier == PreviewTier::Thumbnail ? preview::PreviewCacheTier::Thumbnail : preview::PreviewCacheTier::Standard;
}

// 목적: 요청 tier에 맞는 source preview를 file에서 decode
// 입력: request: source와 target 정보를 포함한 요청, tier: decode 품질 단계
// 출력: develop 전 source QImage 또는 구조화된 오류
[[nodiscard]] preview::ThumbnailPreviewResult decodeSource(const PreviewRequest& request, PreviewTier tier)
{
    if (tier == PreviewTier::Thumbnail)
    {
        return preview::loadFilePreview(request.source, request.targetSize);
    }

    if (request.source.kind != types::SupportedFileKind::Raw)
    {
        return preview::ThumbnailPreviewResult::failure(
            {types::ErrorCode::UnsupportedFormat, QStringLiteral("Standard preview requires a RAW source.")});
    }

    return preview::loadStandardRawPreview(request.source.path, request.targetSize);
}

// 목적: source file version과 preview geometry를 표현하는 memory cache key 생성
// 입력: request: source와 target 정보를 포함한 요청, tier: preview 품질 단계
// 출력: 현재 file metadata에 연결된 cache key
[[nodiscard]] QString makeSourceCacheKey(const PreviewRequest& request, PreviewTier tier)
{
    const QFileInfo fileInfo(QDir::cleanPath(request.source.path));
    const QString absolutePath = fileInfo.absoluteFilePath();

    return QStringLiteral("%1\n%2\n%3\n%4\n%5x%6")
        .arg(absolutePath)
        .arg(fileInfo.size())
        .arg(fileInfo.lastModified().toMSecsSinceEpoch())
        .arg(tier == PreviewTier::Thumbnail ? 0 : 1)
        .arg(request.targetSize.width())
        .arg(request.targetSize.height());
}

// 목적: interactive render의 pixel 수를 줄이되 final render는 cached source를 그대로 유지
// 입력: sourceImage: develop 전 source, renderMode: interactive 또는 final 정책
// 출력: develop에 사용할 implicit-shared 또는 축소된 image
[[nodiscard]] QImage prepareDevelopSource(const QImage& sourceImage, PreviewRenderMode renderMode)
{
    if (renderMode == PreviewRenderMode::Final)
    {
        return sourceImage;
    }

    const QSize interactiveSize{
        std::max(1, sourceImage.width() / InteractivePreviewScaleDivisor),
        std::max(1, sourceImage.height() / InteractivePreviewScaleDivisor),
    };
    return sourceImage.scaled(interactiveSize, Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

// 목적: cooperative cancellation로 중단된 내부 pipeline 결과 생성
// 입력: 없음
// 출력: worker가 terminal failure로 publish하지 않을 Cancelled 오류
[[nodiscard]] PreviewPipelineResult makeCancelledResult()
{
    return PreviewPipelineResult::failure(
        {types::ErrorCode::Cancelled, QStringLiteral("Preview pipeline cancellation requested.")});
}

}  // namespace

class FilePreviewPipeline::Impl
{
public:
    struct CachedSource
    {
        QString key;
        QImage image;
    };

    // 목적: file cache와 worker-local source cache 초기화
    // 입력: cacheRoot: preview cache root directory
    // 출력: 초기화된 implementation 객체
    explicit Impl(QString cacheRoot) : cache(std::move(cacheRoot)) {}

    // 목적: tier별 마지막 source가 현재 cache key와 일치하면 반환
    // 입력: tier: preview 품질 단계, key: 현재 source version key
    // 출력: cache hit이면 implicit-shared QImage, 아니면 빈 값
    [[nodiscard]] std::optional<QImage> findSource(PreviewTier tier, const QString& key) const
    {
        const std::optional<CachedSource>& cached = tier == PreviewTier::Thumbnail ? thumbnailSource : standardSource;

        if (!cached.has_value() || cached->key != key)
        {
            return std::nullopt;
        }

        return cached->image;
    }

    // 목적: tier별 마지막 source image를 bounded worker-local cache에 보관
    // 입력: tier: preview 품질 단계, key: source version key, image: develop 전 source image
    // 출력: 없음
    void rememberSource(PreviewTier tier, QString key, QImage image)
    {
        std::optional<CachedSource>& cached = tier == PreviewTier::Thumbnail ? thumbnailSource : standardSource;
        cached = CachedSource{std::move(key), std::move(image)};
    }

    preview::PreviewCache cache;
    std::optional<CachedSource> thumbnailSource;
    std::optional<CachedSource> standardSource;
};

// 목적: file-backed preview cache를 사용하는 production pipeline 초기화
// 입력: cacheRoot: preview cache root directory
// 출력: 초기화된 pipeline 객체
FilePreviewPipeline::FilePreviewPipeline(QString cacheRoot) : m_impl(std::make_unique<Impl>(std::move(cacheRoot))) {}

// 목적: pipeline implementation resource 정리
// 입력: 없음
// 출력: 없음
FilePreviewPipeline::~FilePreviewPipeline() = default;

// 목적: cache와 file decoder를 통해 요청 tier를 현상하고 분석
// 입력: request: immutable preview 요청, tier: 생성할 preview 품질 단계, cancellationToken: 중단 상태
// 출력: 완성된 frame 또는 구조화된 오류
PreviewPipelineResult FilePreviewPipeline::render(const PreviewRequest& request,
                                                  PreviewTier tier,
                                                  const types::CancellationToken& cancellationToken)
{
    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    PreviewOperationStats stats;
    const Clock::time_point cacheReadStartedAt = Clock::now();
    const QString sourceKey = makeSourceCacheKey(request, tier);
    QImage sourceImage;
    const std::optional<QImage> memorySource = m_impl->findSource(tier, sourceKey);

    if (memorySource.has_value())
    {
        sourceImage = *memorySource;
        stats.sourceCacheHit = true;
    }
    else
    {
        const preview::PreviewCacheLookupResult cached =
            m_impl->cache.load(request.source.path, toCacheTier(tier), request.targetSize);

        if (cached.hasValue() && cached.value().hit)
        {
            sourceImage = cached.value().image;
            stats.sourceCacheHit = true;
            m_impl->rememberSource(tier, sourceKey, sourceImage);
        }
        else if (cached.hasError())
        {
            LOG_WARN("preview", "Preview cache read failed: {}", cached.error().message.toStdString());
        }
    }

    stats.cacheReadMs = elapsedMilliseconds(cacheReadStartedAt);

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    if (sourceImage.isNull())
    {
        const Clock::time_point decodeStartedAt = Clock::now();
        const preview::ThumbnailPreviewResult decoded = decodeSource(request, tier);
        stats.decodeMs = elapsedMilliseconds(decodeStartedAt);

        if (decoded.hasError())
        {
            if (cancellationToken.isCancellationRequested())
            {
                return makeCancelledResult();
            }

            return PreviewPipelineResult::failure(decoded.error());
        }

        sourceImage = decoded.value();
        m_impl->rememberSource(tier, sourceKey, sourceImage);

        if (cancellationToken.isCancellationRequested())
        {
            return makeCancelledResult();
        }

        const Clock::time_point cacheWriteStartedAt = Clock::now();
        const preview::PreviewCacheStoreResult stored =
            m_impl->cache.store(request.source.path, toCacheTier(tier), request.targetSize, sourceImage);
        stats.cacheWriteMs = elapsedMilliseconds(cacheWriteStartedAt);

        if (cancellationToken.isCancellationRequested())
        {
            return makeCancelledResult();
        }

        if (stored.hasError())
        {
            LOG_WARN("preview", "Preview cache write failed: {}", stored.error().message.toStdString());
        }
    }

    const QImage developSource = prepareDevelopSource(sourceImage, request.renderMode);
    const Clock::time_point developStartedAt = Clock::now();
    const develop::DevelopImageResult developed = develop::applyDevelop(developSource, request.params);
    stats.developMs = elapsedMilliseconds(developStartedAt);

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    if (developed.hasError())
    {
        return PreviewPipelineResult::failure(developed.error());
    }

    if (request.renderMode == PreviewRenderMode::Interactive)
    {
        return PreviewPipelineResult::success({developed.value(), {}, {}, stats});
    }

    const Clock::time_point analysisStartedAt = Clock::now();
    const develop::ImageHistogramResult histogram = develop::calculateImageHistogram(developed.value());

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    if (histogram.hasError())
    {
        return PreviewPipelineResult::failure(histogram.error());
    }

    const develop::ClippingSummaryResult clipping = develop::calculateClippingSummary(developed.value());
    stats.analysisMs = elapsedMilliseconds(analysisStartedAt);

    if (cancellationToken.isCancellationRequested())
    {
        return makeCancelledResult();
    }

    if (clipping.hasError())
    {
        return PreviewPipelineResult::failure(clipping.error());
    }

    return PreviewPipelineResult::success({developed.value(), histogram.value(), clipping.value(), stats});
}

}  // namespace flexraw::core::orchestration

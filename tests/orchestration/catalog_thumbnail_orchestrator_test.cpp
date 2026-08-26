#include <atomic>
#include <memory>

#include <QEventLoop>
#include <QSemaphore>
#include <QTimer>

#include <gtest/gtest.h>

#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: thumbnail orchestration test용 raster source descriptor 생성
// 입력: name: source path와 display name을 구분할 값
// 출력: filesystem access가 필요 없는 유효 raster descriptor
[[nodiscard]] types::FileDescriptor makeSource(const QString& name)
{
    return {
        QStringLiteral("C:/thumbnail-test/") + name,
        QStringLiteral("jpg"),
        name,
        types::SupportedFileKind::RasterImage,
    };
}

struct BlockingThumbnailProbe
{
    QSemaphore firstStarted;
    QSemaphore finishFirst;
    std::atomic_int loadCount{0};
};

class BlockingThumbnailPipeline final : public ICatalogThumbnailPipeline
{
public:
    // 목적: test worker와 owner가 공유할 첫 decode blocking state 저장
    // 입력: probe: 시작·release semaphore와 호출 수 state
    // 출력: 첫 요청만 block하는 deterministic pipeline
    explicit BlockingThumbnailPipeline(std::shared_ptr<BlockingThumbnailProbe> probe) : m_probe(std::move(probe)) {}

    // 목적: 첫 decode를 test release까지 유지하고 cancellation 이후 결과 구분
    // 입력: source: 결과 path, targetSize: 미사용, cancellationToken: stale window 상태
    // 출력: current request면 2x1 image, stale이면 Cancelled 오류
    [[nodiscard]] CatalogThumbnailPipelineResult load(const types::FileDescriptor&,
                                                      const QSize&,
                                                      const types::CancellationToken& cancellationToken) override
    {
        if (m_probe->loadCount.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            m_probe->firstStarted.release();
            m_probe->finishFirst.acquire();
        }
        if (cancellationToken.isCancellationRequested())
        {
            return CatalogThumbnailPipelineResult::failure(
                {types::ErrorCode::Cancelled, QStringLiteral("Stale thumbnail window.")});
        }

        QImage image(2, 1, QImage::Format_RGB32);
        image.fill(Qt::green);
        return CatalogThumbnailPipelineResult::success(std::move(image));
    }

private:
    std::shared_ptr<BlockingThumbnailProbe> m_probe;
};

TEST(CatalogThumbnailOrchestratorTest, ReplacesPendingWindowAndFiltersStaleActiveResult)
{
    (void)test::application();
    auto probe = std::make_shared<BlockingThumbnailProbe>();
    CatalogThumbnailOrchestrator orchestrator(std::make_unique<BlockingThumbnailPipeline>(probe));
    QVector<QString> readyPaths;
    QEventLoop eventLoop;
    QObject::connect(&orchestrator,
                     &CatalogThumbnailOrchestrator::thumbnailReady,
                     &eventLoop,
                     [&readyPaths, &eventLoop](const CatalogThumbnailFrame& frame) {
                         readyPaths.push_back(frame.sourcePath);
                         eventLoop.quit();
                     });
    ASSERT_TRUE(orchestrator
                    .updateWindow({{makeSource(QStringLiteral("stale-first.jpg")),
                                    makeSource(QStringLiteral("stale-pending.jpg"))},
                                   QSize{96, 72}})
                    .hasValue());
    const bool firstStarted = probe->firstStarted.tryAcquire(1, 1000);
    if (!firstStarted)
    {
        probe->finishFirst.release();
    }
    ASSERT_TRUE(firstStarted);

    ASSERT_TRUE(orchestrator.updateWindow({{makeSource(QStringLiteral("current.jpg"))}, QSize{96, 72}}).hasValue());
    probe->finishFirst.release();
    QTimer::singleShot(3000, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();

    ASSERT_EQ(1, readyPaths.size());
    EXPECT_EQ(QStringLiteral("C:/thumbnail-test/current.jpg"), readyPaths.front());
    EXPECT_EQ(2, probe->loadCount.load(std::memory_order_relaxed));
}

TEST(CatalogThumbnailOrchestratorTest, RejectsWindowAboveBoundWithoutStartingPipeline)
{
    (void)test::application();
    auto probe = std::make_shared<BlockingThumbnailProbe>();
    CatalogThumbnailOrchestrator orchestrator(std::make_unique<BlockingThumbnailPipeline>(probe));
    CatalogThumbnailWindowRequest request;
    request.targetSize = QSize{96, 72};
    request.sources.reserve(MaximumCatalogThumbnailWindowSize + 1);
    for (int index = 0; index <= MaximumCatalogThumbnailWindowSize; ++index)
    {
        request.sources.push_back(makeSource(QStringLiteral("photo-%1.jpg").arg(index)));
    }

    const CatalogThumbnailWindowResult result = orchestrator.updateWindow(std::move(request));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
    EXPECT_EQ(0, probe->loadCount.load(std::memory_order_relaxed));
}

}  // namespace
}  // namespace flexraw::core::orchestration

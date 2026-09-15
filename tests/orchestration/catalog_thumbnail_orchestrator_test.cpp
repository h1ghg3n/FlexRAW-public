#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: thumbnail orchestration test용 transient Qt-free item 생성
// 입력: name: source locator와 display name을 구분할 값
// 출력: filesystem access가 필요 없는 normalized raster item
[[nodiscard]] client::CatalogThumbnailItem makeItem(const QString& name)
{
    const QString path = QDir::cleanPath(QDir(QDir::tempPath()).filePath(QStringLiteral("thumbnail-test/") + name));
    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray nameUtf8 = name.toUtf8();
    const std::string locator(pathUtf8.constData(), static_cast<std::size_t>(pathUtf8.size()));
    return {{client::CatalogThumbnailIdentityKind::TransientSource, {}, locator},
            locator,
            "jpg",
            {nameUtf8.constData(), static_cast<std::size_t>(nameUtf8.size())},
            client::CatalogFileKind::RasterImage};
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

struct ThumbnailDestructionProbe
{
    QSemaphore started;
    QSemaphore cancellationObserved;
    std::atomic_bool loadReturned{false};
};

class CancellationBlockingThumbnailPipeline final : public ICatalogThumbnailPipeline
{
public:
    // 목적: Orchestrator destruction test와 cancellation 관찰 state 공유
    // 입력: probe: 시작·취소·반환 상태를 보유한 shared probe
    // 출력: cancellation 전까지 bounded 대기하는 pipeline
    explicit CancellationBlockingThumbnailPipeline(std::shared_ptr<ThumbnailDestructionProbe> probe)
        : m_probe(std::move(probe))
    {}

    // 목적: active load가 owner cancellation을 관찰한 뒤에만 정상적으로 반환하는지 검증
    // 입력: source/targetSize: 미사용, cancellationToken: Orchestrator-owned cancellation state
    // 출력: cancellation이면 Cancelled, 제한 시간 초과면 Unknown test failure
    [[nodiscard]] CatalogThumbnailPipelineResult load(const types::FileDescriptor&,
                                                      const QSize&,
                                                      const types::CancellationToken& cancellationToken) override
    {
        m_probe->started.release();
        for (int attempt = 0; attempt < 600; ++attempt)
        {
            if (cancellationToken.isCancellationRequested())
            {
                m_probe->cancellationObserved.release();
                m_probe->loadReturned.store(true, std::memory_order_relaxed);
                return CatalogThumbnailPipelineResult::failure(
                    {types::ErrorCode::Cancelled, QStringLiteral("Thumbnail owner was destroyed.")});
            }
            QThread::msleep(5);
        }
        m_probe->loadReturned.store(true, std::memory_order_relaxed);
        return CatalogThumbnailPipelineResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Thumbnail destruction cancellation timed out.")});
    }

private:
    std::shared_ptr<ThumbnailDestructionProbe> m_probe;
};

TEST(CatalogThumbnailOrchestratorTest, ReplacesPendingWindowAndFiltersStaleActiveResult)
{
    (void)test::application();
    auto probe = std::make_shared<BlockingThumbnailProbe>();
    CatalogThumbnailOrchestrator orchestrator(std::make_unique<BlockingThumbnailPipeline>(probe));
    std::vector<CatalogThumbnailFrame> frames;
    std::vector<CatalogThumbnailWindowTerminal> terminals;
    QEventLoop eventLoop;
    QObject::connect(&orchestrator,
                     &CatalogThumbnailOrchestrator::thumbnailReady,
                     &eventLoop,
                     [&frames](const CatalogThumbnailFrame& frame) { frames.push_back(frame); });
    QObject::connect(&orchestrator,
                     &CatalogThumbnailOrchestrator::thumbnailWindowTerminal,
                     &eventLoop,
                     [&terminals, &eventLoop](const CatalogThumbnailWindowTerminal& terminal) {
                         terminals.push_back(terminal);
                         if (terminal.state == client::CatalogThumbnailTerminalState::Completed)
                         {
                             eventLoop.quit();
                         }
                     });
    const client::CatalogThumbnailWindowResult stale = orchestrator.replaceThumbnailWindow(
        {{makeItem(QStringLiteral("stale-first.jpg")), makeItem(QStringLiteral("stale-pending.jpg"))}, {96, 72}});
    ASSERT_TRUE(stale.hasValue());
    const bool firstStarted = probe->firstStarted.tryAcquire(1, 1000);
    if (!firstStarted)
    {
        probe->finishFirst.release();
    }
    ASSERT_TRUE(firstStarted);

    const client::CatalogThumbnailWindowResult current =
        orchestrator.replaceThumbnailWindow({{makeItem(QStringLiteral("current.jpg"))}, {96, 72}});
    ASSERT_TRUE(current.hasValue());
    probe->finishFirst.release();
    QTimer::singleShot(3000, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();

    ASSERT_EQ(1U, frames.size());
    EXPECT_EQ(current.value().generation, frames.front().generation);
    EXPECT_EQ(makeItem(QStringLiteral("current.jpg")).identity, frames.front().identity);
    ASSERT_EQ(2U, terminals.size());
    EXPECT_EQ(stale.value().generation, terminals.front().generation);
    EXPECT_EQ(client::CatalogThumbnailTerminalState::Cancelled, terminals.front().state);
    EXPECT_EQ(current.value().generation, terminals.back().generation);
    EXPECT_EQ(client::CatalogThumbnailTerminalState::Completed, terminals.back().state);
    EXPECT_EQ(2, probe->loadCount.load(std::memory_order_relaxed));
}

TEST(CatalogThumbnailOrchestratorTest, RejectsWindowAboveBoundWithoutStartingPipeline)
{
    (void)test::application();
    auto probe = std::make_shared<BlockingThumbnailProbe>();
    CatalogThumbnailOrchestrator orchestrator(std::make_unique<BlockingThumbnailPipeline>(probe));
    client::ReplaceCatalogThumbnailWindowCommand command;
    command.targetExtent = {96, 72};
    command.items.reserve(client::MaximumCatalogThumbnailWindowSize + 1);
    for (std::size_t index = 0; index <= client::MaximumCatalogThumbnailWindowSize; ++index)
    {
        command.items.push_back(makeItem(QStringLiteral("photo-%1.jpg").arg(index)));
    }

    const client::CatalogThumbnailWindowResult result = orchestrator.replaceThumbnailWindow(command);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(client::ClientErrorCode::InvalidArgument, result.error().code);
    EXPECT_EQ(0, probe->loadCount.load(std::memory_order_relaxed));
}

TEST(CatalogThumbnailOrchestratorTest, RejectsNonNormalizedLocatorAndMismatchedTransientIdentity)
{
    (void)test::application();
    auto probe = std::make_shared<BlockingThumbnailProbe>();
    CatalogThumbnailOrchestrator orchestrator(std::make_unique<BlockingThumbnailPipeline>(probe));
    client::CatalogThumbnailItem nonNormalized = makeItem(QStringLiteral("photo.jpg"));
    nonNormalized.sourceLocator += "/../photo.jpg";
    client::CatalogThumbnailItem mismatchedIdentity = makeItem(QStringLiteral("other.jpg"));
    mismatchedIdentity.identity.transientSourceLocator += ".different";

    const client::CatalogThumbnailWindowResult nonNormalizedResult =
        orchestrator.replaceThumbnailWindow({{nonNormalized}, {96, 72}});
    const client::CatalogThumbnailWindowResult mismatchedResult =
        orchestrator.replaceThumbnailWindow({{mismatchedIdentity}, {96, 72}});

    ASSERT_TRUE(nonNormalizedResult.hasError());
    EXPECT_EQ(client::ClientErrorCode::InvalidArgument, nonNormalizedResult.error().code);
    ASSERT_TRUE(mismatchedResult.hasError());
    EXPECT_EQ(client::ClientErrorCode::InvalidArgument, mismatchedResult.error().code);
    EXPECT_EQ(0, probe->loadCount.load(std::memory_order_relaxed));
}

TEST(CatalogThumbnailOrchestratorTest, ClearsCurrentWindowOnceWithCancelledTerminal)
{
    (void)test::application();
    auto probe = std::make_shared<BlockingThumbnailProbe>();
    CatalogThumbnailOrchestrator orchestrator(std::make_unique<BlockingThumbnailPipeline>(probe));
    std::vector<CatalogThumbnailWindowTerminal> terminals;
    QObject::connect(&orchestrator,
                     &CatalogThumbnailOrchestrator::thumbnailWindowTerminal,
                     &orchestrator,
                     [&terminals](const CatalogThumbnailWindowTerminal& terminal) { terminals.push_back(terminal); });
    const client::CatalogThumbnailWindowResult receipt =
        orchestrator.replaceThumbnailWindow({{makeItem(QStringLiteral("active.jpg"))}, {96, 72}});
    ASSERT_TRUE(receipt.hasValue());
    const bool started = probe->firstStarted.tryAcquire(1, 1000);
    if (!started)
    {
        probe->finishFirst.release();
    }
    ASSERT_TRUE(started);

    EXPECT_TRUE(orchestrator.clearThumbnailWindow().hasValue());
    EXPECT_TRUE(orchestrator.clearThumbnailWindow().hasValue());

    ASSERT_EQ(1U, terminals.size());
    EXPECT_EQ(receipt.value().generation, terminals.front().generation);
    EXPECT_EQ(client::CatalogThumbnailTerminalState::Cancelled, terminals.front().state);
    EXPECT_FALSE(orchestrator.thumbnailWindowSnapshot().activeGeneration.has_value());
    probe->finishFirst.release();
}

TEST(CatalogThumbnailOrchestratorTest, DestructionCancelsAndWaitsForInFlightPipeline)
{
    (void)test::application();
    auto probe = std::make_shared<ThumbnailDestructionProbe>();
    auto orchestrator =
        std::make_unique<CatalogThumbnailOrchestrator>(std::make_unique<CancellationBlockingThumbnailPipeline>(probe));
    const client::CatalogThumbnailWindowResult receipt =
        orchestrator->replaceThumbnailWindow({{makeItem(QStringLiteral("active-destruction.jpg"))}, {96, 72}});
    ASSERT_TRUE(receipt.hasValue());
    ASSERT_TRUE(probe->started.tryAcquire(1, 1000));

    orchestrator.reset();

    EXPECT_TRUE(probe->cancellationObserved.tryAcquire(1, 1000));
    EXPECT_TRUE(probe->loadReturned.load(std::memory_order_relaxed));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

}  // namespace
}  // namespace flexraw::core::orchestration

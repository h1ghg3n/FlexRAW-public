#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: image plugin 없이 raster preview test용 BMP 파일 생성
// 입력: path: 생성할 BMP 파일 경로
// 출력: 파일 생성 성공 여부
[[nodiscard]] bool createBitmapFile(const QString& path)
{
    const QByteArray bitmap =
        QByteArray::fromHex("424d3e0000000000000036000000"
                            "28000000020000000100000001001800000000000800000000000000000000000000000000000000"
                            "ff0000ff00000000");
    QFile file(path);

    if (!file.open(QIODevice::WriteOnly))
    {
        return false;
    }

    return file.write(bitmap) == bitmap.size();
}

// 목적: orchestration contract test용 raster request 생성
// 입력: imagePath: source BMP 경로, revision: develop revision, sequence: editor preview 순서
// 출력: 유효한 PreviewRequest 값
[[nodiscard]] PreviewRequest makeRasterRequest(const QString& imagePath,
                                               types::DevelopRevision revision,
                                               types::PreviewSequence sequence)
{
    return PreviewRequest{
        {{}, imagePath, revision},
        {
            imagePath,
            QStringLiteral("bmp"),
            QStringLiteral("sample.bmp"),
            types::SupportedFileKind::RasterImage,
        },
        QSize{2, 2},
        {},
        sequence,
    };
}

class BlockingPreviewPipeline final : public IPreviewPipeline
{
public:
    // 목적: 첫 render가 test release 전까지 worker thread를 점유하도록 제어
    // 입력: request: 사용하지 않는 request 값, tier: 반환 frame의 tier, cancellationToken: 사용하지 않는 token
    // 출력: 1x1 test frame
    [[nodiscard]] PreviewPipelineResult render(const PreviewRequest&,
                                               PreviewTier,
                                               const types::CancellationToken&) override
    {
        if (m_renderCount.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            firstRenderStarted.release();
            allowFirstRenderToFinish.acquire();
        }

        QImage image(1, 1, QImage::Format_RGBA8888);
        image.fill(Qt::black);
        develop::ImageHistogram histogram;
        histogram.pixelCount = 1;
        develop::ClippingSummary clipping;
        clipping.shadowPixelCount = 1;
        clipping.pixelCount = 1;
        return PreviewPipelineResult::success({image, histogram, clipping, {}});
    }

    QSemaphore firstRenderStarted;
    QSemaphore allowFirstRenderToFinish;

private:
    std::atomic_int m_renderCount{0};
};

class ProgressivePreviewPipeline final : public IPreviewPipeline
{
public:
    // 목적: 요청 tier를 pixel color로 구분하는 deterministic progressive frame 생성
    // 입력: request: 사용하지 않는 request 값, tier: 생성할 progressive 품질 단계, cancellationToken: 미사용
    // 출력: tier별 color를 가진 1x1 test frame
    [[nodiscard]] PreviewPipelineResult render(const PreviewRequest&,
                                               PreviewTier tier,
                                               const types::CancellationToken&) override
    {
        QImage image(1, 1, QImage::Format_RGBA8888);
        image.fill(tier == PreviewTier::Thumbnail ? Qt::red : Qt::green);
        develop::ImageHistogram histogram;
        histogram.pixelCount = 1;
        develop::ClippingSummary clipping;
        clipping.pixelCount = 1;
        return PreviewPipelineResult::success({image, histogram, clipping, {}});
    }
};

struct CancellationProbe
{
    QSemaphore renderStarted;
    QSemaphore cancellationObserved;
    std::atomic_bool timedOut{false};
};

class CancellationAwarePreviewPipeline final : public IPreviewPipeline
{
public:
    // 목적: test와 worker가 공유할 cancellation 관찰 state 저장
    // 입력: probe: render 시작, 취소 관찰과 timeout을 기록할 shared state
    // 출력: cooperative cancellation test pipeline
    explicit CancellationAwarePreviewPipeline(std::shared_ptr<CancellationProbe> probe) : m_probe(std::move(probe)) {}

    // 목적: worker에서 취소 token을 관찰할 때까지 bounded polling 수행
    // 입력: request: 사용하지 않는 request, tier: 사용하지 않는 tier, cancellationToken: 관찰할 token
    // 출력: 취소 관찰 시 Cancelled, 제한 시간 초과 시 Unknown 오류
    [[nodiscard]] PreviewPipelineResult render(const PreviewRequest&,
                                               PreviewTier,
                                               const types::CancellationToken& cancellationToken) override
    {
        m_probe->renderStarted.release();

        for (int attempt = 0; attempt < 5000; ++attempt)
        {
            if (cancellationToken.isCancellationRequested())
            {
                m_probe->cancellationObserved.release();
                return PreviewPipelineResult::failure(
                    {types::ErrorCode::Cancelled, QStringLiteral("Cancellation observed by test pipeline.")});
            }

            QThread::msleep(1);
        }

        m_probe->timedOut.store(true, std::memory_order_release);
        return PreviewPipelineResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Timed out waiting for cancellation.")});
    }

private:
    std::shared_ptr<CancellationProbe> m_probe;
};

TEST(PreviewPipelineTest, StopsBeforeFilesystemAccessWhenAlreadyCancelled)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    FilePreviewPipeline pipeline(QDir(directory.path()).filePath(QStringLiteral("cache")));
    types::CancellationSource cancellationSource;
    cancellationSource.requestCancellation();
    const PreviewRequest request = makeRasterRequest(QStringLiteral("C:/missing/cancelled.bmp"), 0, 0);

    const PreviewPipelineResult result = pipeline.render(request, PreviewTier::Thumbnail, cancellationSource.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::Cancelled, result.error().code);
}

TEST(PreviewPipelineTest, DownscalesInteractiveFrameAndSkipsAnalysis)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString imagePath = QDir(directory.path()).filePath(QStringLiteral("interactive.bmp"));
    ASSERT_TRUE(createBitmapFile(imagePath));
    FilePreviewPipeline pipeline(QDir(directory.path()).filePath(QStringLiteral("cache")));
    PreviewRequest request = makeRasterRequest(imagePath, 1, 1);
    request.renderMode = PreviewRenderMode::Interactive;
    const types::CancellationSource cancellationSource;

    const PreviewPipelineResult result = pipeline.render(request, PreviewTier::Thumbnail, cancellationSource.token());

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(result.value().image.size(), QSize(1, 1));
    EXPECT_EQ(result.value().histogram.pixelCount, 0U);
    EXPECT_EQ(result.value().clipping.pixelCount, 0U);
    EXPECT_EQ(result.value().stats.analysisMs, 0);
}

TEST(PreviewOrchestratorTest, RunsRasterPipelineOffThreadAndPublishesAnalyzedFrame)
{
    (void)test::application();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString imagePath = QDir(directory.path()).filePath(QStringLiteral("sample.bmp"));
    ASSERT_TRUE(createBitmapFile(imagePath));
    PreviewOrchestrator orchestrator(
        std::make_unique<FilePreviewPipeline>(QDir(directory.path()).filePath(QStringLiteral("cache"))));
    QEventLoop eventLoop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    std::vector<PreviewResult> frames;
    int completedCount = 0;
    int failedCount = 0;

    QObject::connect(&orchestrator, &PreviewOrchestrator::previewUpdated, [&frames](const PreviewResult& result) {
        frames.push_back(result);
    });
    QObject::connect(&orchestrator, &PreviewOrchestrator::previewCompleted, [&](types::RequestId) {
        ++completedCount;
        eventLoop.quit();
    });
    QObject::connect(&orchestrator, &PreviewOrchestrator::previewFailed, [&](const PreviewIssue&) {
        ++failedCount;
        eventLoop.quit();
    });
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);

    const PreviewSubmissionResult submitted = orchestrator.submitPreview(makeRasterRequest(imagePath, 3, 7));
    ASSERT_TRUE(submitted.hasValue());
    timeoutTimer.start(5000);
    eventLoop.exec();

    ASSERT_EQ(1, completedCount);
    EXPECT_EQ(0, failedCount);
    ASSERT_EQ(1U, frames.size());
    EXPECT_EQ(submitted.value(), frames.front().requestId);
    EXPECT_EQ(imagePath, frames.front().photo.transientKey);
    EXPECT_EQ(3U, frames.front().photo.developRevision);
    EXPECT_EQ(7U, frames.front().previewSequence);
    EXPECT_EQ(PreviewTier::Thumbnail, frames.front().tier);
    EXPECT_EQ(QSize(2, 1), frames.front().image.size());
    EXPECT_EQ(QColor(Qt::blue), frames.front().image.pixelColor(0, 0));
    EXPECT_EQ(PreviewRenderMode::Final, frames.front().renderMode);
    EXPECT_EQ(2U, frames.front().histogram.pixelCount);
    EXPECT_EQ(2U, frames.front().clipping.pixelCount);
    EXPECT_GE(frames.front().stats.queueWaitMs, 0);
    EXPECT_GE(frames.front().stats.totalMs, 0);
}

TEST(PreviewOrchestratorTest, CancelsBlockedRequestAndOnlyPublishesLatestRequest)
{
    (void)test::application();
    auto pipeline = std::make_unique<BlockingPreviewPipeline>();
    BlockingPreviewPipeline* const pipelineControl = pipeline.get();
    PreviewOrchestrator orchestrator(std::move(pipeline));
    const QString imagePath = QStringLiteral("C:/virtual/sample.bmp");
    std::vector<PreviewResult> frames;
    std::vector<types::RequestId> cancelledRequests;
    std::vector<types::RequestId> completedRequests;
    int failedCount = 0;
    QEventLoop eventLoop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);

    QObject::connect(&orchestrator, &PreviewOrchestrator::previewUpdated, [&frames](const PreviewResult& result) {
        frames.push_back(result);
    });
    QObject::connect(&orchestrator,
                     &PreviewOrchestrator::previewCancelled,
                     [&cancelledRequests](types::RequestId requestId) { cancelledRequests.push_back(requestId); });
    QObject::connect(&orchestrator, &PreviewOrchestrator::previewCompleted, [&](types::RequestId requestId) {
        completedRequests.push_back(requestId);
        eventLoop.quit();
    });
    QObject::connect(&orchestrator, &PreviewOrchestrator::previewFailed, [&](const PreviewIssue&) {
        ++failedCount;
        eventLoop.quit();
    });
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);

    const PreviewSubmissionResult first = orchestrator.submitPreview(makeRasterRequest(imagePath, 1, 10));
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(pipelineControl->firstRenderStarted.tryAcquire(1, 5000));
    EXPECT_TRUE(orchestrator.cancelPreview(first.value()));
    const PreviewSubmissionResult second = orchestrator.submitPreview(makeRasterRequest(imagePath, 2, 11));
    ASSERT_TRUE(second.hasValue());
    pipelineControl->allowFirstRenderToFinish.release();
    timeoutTimer.start(5000);
    eventLoop.exec();

    ASSERT_EQ(1U, cancelledRequests.size());
    EXPECT_EQ(first.value(), cancelledRequests.front());
    ASSERT_EQ(1U, completedRequests.size());
    EXPECT_EQ(second.value(), completedRequests.front());
    EXPECT_EQ(0, failedCount);
    ASSERT_EQ(1U, frames.size());
    EXPECT_EQ(second.value(), frames.front().requestId);
    EXPECT_EQ(2U, frames.front().photo.developRevision);
    EXPECT_EQ(11U, frames.front().previewSequence);
}

TEST(PreviewOrchestratorTest, PropagatesExplicitCancellationIntoRunningPipeline)
{
    (void)test::application();
    const auto probe = std::make_shared<CancellationProbe>();
    PreviewOrchestrator orchestrator(std::make_unique<CancellationAwarePreviewPipeline>(probe));
    const PreviewSubmissionResult submitted =
        orchestrator.submitPreview(makeRasterRequest(QStringLiteral("C:/virtual/cancel.bmp"), 1, 1));
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(probe->renderStarted.tryAcquire(1, 5000));

    EXPECT_TRUE(orchestrator.cancelPreview(submitted.value()));

    EXPECT_TRUE(probe->cancellationObserved.tryAcquire(1, 5000));
    EXPECT_FALSE(probe->timedOut.load(std::memory_order_acquire));
}

TEST(PreviewOrchestratorTest, CancelsActivePipelineBeforeWaitingForShutdown)
{
    (void)test::application();
    const auto probe = std::make_shared<CancellationProbe>();

    {
        PreviewOrchestrator orchestrator(std::make_unique<CancellationAwarePreviewPipeline>(probe));
        const PreviewSubmissionResult submitted =
            orchestrator.submitPreview(makeRasterRequest(QStringLiteral("C:/virtual/shutdown.bmp"), 1, 1));
        ASSERT_TRUE(submitted.hasValue());
        ASSERT_TRUE(probe->renderStarted.tryAcquire(1, 5000));
    }

    EXPECT_TRUE(probe->cancellationObserved.tryAcquire(1, 5000));
    EXPECT_FALSE(probe->timedOut.load(std::memory_order_acquire));
}

TEST(PreviewOrchestratorTest, StreamsRawThumbnailBeforeStandardAndCompletesOnce)
{
    (void)test::application();
    PreviewOrchestrator orchestrator(std::make_unique<ProgressivePreviewPipeline>());
    PreviewRequest request = makeRasterRequest(QStringLiteral("C:/virtual/sample.cr3"), 4, 12);
    request.source.extension = QStringLiteral("cr3");
    request.source.displayName = QStringLiteral("sample.cr3");
    request.source.kind = types::SupportedFileKind::Raw;
    std::vector<PreviewResult> frames;
    int completedCount = 0;
    int failedCount = 0;
    QEventLoop eventLoop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);

    QObject::connect(&orchestrator, &PreviewOrchestrator::previewUpdated, [&frames](const PreviewResult& result) {
        frames.push_back(result);
    });
    QObject::connect(&orchestrator, &PreviewOrchestrator::previewCompleted, [&](types::RequestId) {
        ++completedCount;
        eventLoop.quit();
    });
    QObject::connect(&orchestrator, &PreviewOrchestrator::previewFailed, [&](const PreviewIssue&) {
        ++failedCount;
        eventLoop.quit();
    });
    QObject::connect(&timeoutTimer, &QTimer::timeout, &eventLoop, &QEventLoop::quit);

    const PreviewSubmissionResult submitted = orchestrator.submitPreview(std::move(request));
    ASSERT_TRUE(submitted.hasValue());
    timeoutTimer.start(5000);
    eventLoop.exec();

    EXPECT_EQ(1, completedCount);
    EXPECT_EQ(0, failedCount);
    ASSERT_EQ(2U, frames.size());
    EXPECT_EQ(PreviewTier::Thumbnail, frames[0].tier);
    EXPECT_EQ(QColor(Qt::red), frames[0].image.pixelColor(0, 0));
    EXPECT_EQ(PreviewTier::Standard, frames[1].tier);
    EXPECT_EQ(QColor(Qt::green), frames[1].image.pixelColor(0, 0));
}

TEST(PreviewOrchestratorTest, RejectsInvalidRequestBeforeQueueing)
{
    (void)test::application();
    auto pipeline = std::make_unique<BlockingPreviewPipeline>();
    PreviewOrchestrator orchestrator(std::move(pipeline));
    PreviewRequest request;

    const PreviewSubmissionResult submitted = orchestrator.submitPreview(request);

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, submitted.error().code);
}

TEST(PreviewOrchestratorTest, RejectsAmbiguousCatalogAndTransientIdentity)
{
    (void)test::application();
    auto pipeline = std::make_unique<BlockingPreviewPipeline>();
    PreviewOrchestrator orchestrator(std::move(pipeline));
    PreviewRequest request = makeRasterRequest(QStringLiteral("C:/virtual/ambiguous.bmp"), 0, 0);
    request.photo.photoId = types::PhotoId{7};

    const PreviewSubmissionResult submitted = orchestrator.submitPreview(request);

    ASSERT_TRUE(submitted.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, submitted.error().code);
}

}  // namespace
}  // namespace flexraw::core::orchestration

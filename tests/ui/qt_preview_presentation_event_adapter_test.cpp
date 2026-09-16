#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QImage>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "editor_orchestrator.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "qt_preview_presentation_event_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

class PresentationPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: Preview presentation adapter test용 deterministic frame과 analysis 생성
    // 입력: request/tier/cancellationToken: identity와 cancellation contract를 위한 context
    // 출력: known-color 2x1 image와 fixed analysis count
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(
        const core::orchestration::PreviewRequest&,
        core::orchestration::PreviewTier,
        const core::types::CancellationToken& cancellationToken) override
    {
        if (cancellationToken.isCancellationRequested())
        {
            return core::orchestration::PreviewPipelineResult::failure(
                {core::types::ErrorCode::Cancelled, QStringLiteral("Presentation test request was cancelled.")});
        }
        QImage image(2, 1, QImage::Format_RGBA8888);
        image.setPixelColor(0, 0, QColor(10, 20, 30, 40));
        image.setPixelColor(1, 0, QColor(50, 60, 70, 80));
        core::develop::ImageHistogram histogram;
        histogram.red[10] = 2;
        histogram.pixelCount = 2;
        core::develop::ClippingSummary clipping;
        clipping.shadowPixelCount = 1;
        clipping.highlightPixelCount = 1;
        clipping.pixelCount = 2;
        return core::orchestration::PreviewPipelineResult::success(
            {std::move(image), std::move(histogram), clipping, {}});
    }
};

struct ProgressiveCancellationProbe
{
    QSemaphore standardStarted;
    QSemaphore cancellationObserved;
    std::atomic_bool timedOut{false};
};

class ProgressiveCancellationPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: progressive frame 뒤 running Standard 취소를 관찰할 shared state 저장
    // 입력: probe: Standard 시작·취소 관찰과 timeout state
    // 출력: cancellation-aware progressive test pipeline
    explicit ProgressiveCancellationPipeline(std::shared_ptr<ProgressiveCancellationProbe> probe)
        : m_probe(std::move(probe))
    {}

    // 목적: Thumbnail은 즉시 발행하고 Standard는 cooperative cancellation까지 대기
    // 입력: request: 미사용, tier: progressive 단계, cancellationToken: Standard 취소 관찰 token
    // 출력: Thumbnail frame 또는 cancellation failure
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(
        const core::orchestration::PreviewRequest&,
        core::orchestration::PreviewTier tier,
        const core::types::CancellationToken& cancellationToken) override
    {
        if (tier == core::orchestration::PreviewTier::Thumbnail)
        {
            QImage image(2, 1, QImage::Format_RGBA8888);
            image.fill(Qt::red);
            core::develop::ImageHistogram histogram;
            histogram.pixelCount = 2;
            core::develop::ClippingSummary clipping;
            clipping.pixelCount = 2;
            return core::orchestration::PreviewPipelineResult::success({image, histogram, clipping, {}});
        }

        m_probe->standardStarted.release();
        for (int attempt = 0; attempt < 5000; ++attempt)
        {
            if (cancellationToken.isCancellationRequested())
            {
                m_probe->cancellationObserved.release();
                return core::orchestration::PreviewPipelineResult::failure(
                    {core::types::ErrorCode::Cancelled, QStringLiteral("Cancellation observed by presentation test.")});
            }
            QThread::msleep(1);
        }

        m_probe->timedOut.store(true, std::memory_order_release);
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Unknown, QStringLiteral("Timed out waiting for presentation cancellation.")});
    }

private:
    std::shared_ptr<ProgressiveCancellationProbe> m_probe;
};

class ProjectionFailureAfterFramePipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: valid Thumbnail 뒤 DisplayFrame으로 변환할 수 없는 Standard image 생성
    // 입력: request/cancellationToken: 미사용, tier: valid 또는 null image 단계
    // 출력: pipeline 관점에서는 두 tier 모두 성공한 frame
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier tier,
                                                                    const core::types::CancellationToken&) override
    {
        QImage image;
        if (tier == core::orchestration::PreviewTier::Thumbnail)
        {
            image = QImage(2, 1, QImage::Format_RGBA8888);
            image.fill(Qt::red);
        }
        core::develop::ImageHistogram histogram;
        histogram.pixelCount = image.isNull() ? 0 : 2;
        core::develop::ClippingSummary clipping;
        clipping.pixelCount = histogram.pixelCount;
        return core::orchestration::PreviewPipelineResult::success({image, histogram, clipping, {}});
    }
};

// 목적: Preview presentation test source file 생성
// 입력: path: temporary source 경로
// 출력: test bytes 전체 기록 성공 여부
[[nodiscard]] bool writeSourceFile(const QString& path)
{
    QFile file(path);
    const QByteArray contents("preview-presentation-source");
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
           file.flush();
}

// 목적: Qt event를 처리하며 Preview presentation predicate를 bounded wait
// 입력: condition: event collection 상태 predicate, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 condition이 true이면 true
[[nodiscard]] bool waitForCondition(const std::function<bool()>& condition, int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs)
    {
        if (condition())
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return condition();
}

TEST(QtPreviewPresentationEventAdapterTest, DeliversInitialSnapshotAndValidatesViewportCommand)
{
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<PresentationPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    QtPreviewPresentationEventAdapter adapter(editorOrchestrator);
    std::vector<core::client::PreviewPresentationEvent> events;
    bool subscribeReturned = false;
    bool reenteredSubscribe = false;

    const core::client::PreviewPresentationSubscriptionResult subscription = adapter.subscribeToPreviewPresentation(
        [&events, &subscribeReturned, &reenteredSubscribe](const core::client::PreviewPresentationEvent& event) {
            reenteredSubscribe = reenteredSubscribe || !subscribeReturned;
            events.push_back(event);
        });
    subscribeReturned = true;

    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForCondition([&events] { return events.size() == 1; }));
    EXPECT_FALSE(reenteredSubscribe);
    EXPECT_TRUE(events.front().initial);
    EXPECT_FALSE(events.front().snapshot.viewport.has_value());
    EXPECT_FALSE(events.front().snapshot.selectedPhotoId.has_value());
    EXPECT_FALSE(events.front().snapshot.currentFrame.has_value());
    EXPECT_TRUE(editorOrchestrator.setPreviewViewport({{0, 480}}).hasError());
    ASSERT_TRUE(editorOrchestrator.setPreviewViewport({{800, 600}}).hasValue());
    ASSERT_TRUE(waitForCondition([&events] { return events.size() >= 2; }));
    ASSERT_TRUE(events.back().snapshot.viewport.has_value());
    EXPECT_EQ(core::client::PreviewViewport({800, 600}), *events.back().snapshot.viewport);
}

TEST(QtPreviewPresentationEventAdapterTest, PublishesFrameAnalysisAndExactCompletedTerminal)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("presented.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("presented.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<PresentationPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    QtPreviewPresentationEventAdapter adapter(editorOrchestrator);
    std::vector<core::client::PreviewPresentationEvent> events;
    const core::client::PreviewPresentationSubscriptionResult subscription = adapter.subscribeToPreviewPresentation(
        [&events](const core::client::PreviewPresentationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    const core::catalog::CatalogEntry entry{
        {sourcePath,
         QStringLiteral("jpg"),
         QStringLiteral("presented.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(editorOrchestrator.activatePhoto(entry, QSize{640, 480}).hasValue());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(events.cbegin(), events.cend(), [](const auto& event) {
            return event.terminal.has_value() && event.terminal->state == core::client::PreviewTerminalState::Completed;
        });
    }));

    const auto started =
        std::find_if(events.cbegin(), events.cend(), [](const auto& event) { return event.requestStarted; });
    const auto frame =
        std::find_if(events.cbegin(), events.cend(), [](const auto& event) { return event.frameUpdated; });
    const auto terminal = std::find_if(events.cbegin(), events.cend(), [](const auto& event) {
        return event.terminal.has_value() && event.terminal->state == core::client::PreviewTerminalState::Completed;
    });
    ASSERT_NE(events.cend(), started);
    ASSERT_NE(events.cend(), frame);
    ASSERT_NE(events.cend(), terminal);
    ASSERT_TRUE(started->snapshot.activeRequestId.has_value());
    ASSERT_TRUE(frame->snapshot.currentFrame.has_value());
    ASSERT_TRUE(terminal->terminal->requestId.has_value());
    EXPECT_EQ(started->snapshot.activeRequestId, terminal->terminal->requestId);
    EXPECT_EQ(started->snapshot.selectedPhotoId, terminal->terminal->photoId);
    EXPECT_EQ(started->snapshot.developRevision, terminal->terminal->developRevision);
    EXPECT_FALSE(terminal->snapshot.activeRequestId.has_value());
    EXPECT_EQ(core::client::PreviewFrameTier::Thumbnail, frame->snapshot.currentFrame->tier);
    EXPECT_EQ(core::client::PreviewPresentationMode::Final, frame->snapshot.currentFrame->mode);
    EXPECT_EQ(2U, frame->snapshot.currentFrame->frame.width());
    EXPECT_EQ(1U, frame->snapshot.currentFrame->frame.height());
    EXPECT_EQ(30U, frame->snapshot.currentFrame->frame.bytes()[0]);
    ASSERT_TRUE(frame->snapshot.currentFrame->analysis.has_value());
    EXPECT_EQ(2U, frame->snapshot.currentFrame->analysis->histogram.pixelCount);
    EXPECT_EQ(2U, frame->snapshot.currentFrame->analysis->histogram.red[10]);
    EXPECT_EQ(1U, frame->snapshot.currentFrame->analysis->clipping.shadowPixelCount);
    EXPECT_LT(started->sequence.value, frame->sequence.value);
    EXPECT_LT(frame->sequence.value, terminal->sequence.value);
}

TEST(QtPreviewPresentationEventAdapterTest, OmitsUncomputedInteractiveAnalysisAndRestoresFinalAnalysis)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("interactive.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("interactive.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<PresentationPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    QtPreviewPresentationEventAdapter adapter(editorOrchestrator);
    std::vector<core::client::PreviewPresentationEvent> events;
    const core::client::PreviewPresentationSubscriptionResult subscription = adapter.subscribeToPreviewPresentation(
        [&events](const core::client::PreviewPresentationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    const core::catalog::CatalogEntry entry{
        {sourcePath,
         QStringLiteral("jpg"),
         QStringLiteral("interactive.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(editorOrchestrator.activatePhoto(entry, QSize{640, 480}).hasValue());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(events.cbegin(), events.cend(), [](const auto& event) {
            return event.terminal.has_value() && event.terminal->state == core::client::PreviewTerminalState::Completed;
        });
    }));
    editorOrchestrator.beginEdit();
    core::types::DevelopParams params;
    params.exposureEv = 0.25F;
    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(params, QSize{640, 480}));
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(events.cbegin(), events.cend(), [](const auto& event) {
            return event.frameUpdated && event.snapshot.currentFrame.has_value() &&
                   event.snapshot.currentFrame->mode == core::client::PreviewPresentationMode::Interactive;
        });
    }));

    const auto interactiveFrame = std::find_if(events.cbegin(), events.cend(), [](const auto& event) {
        return event.frameUpdated && event.snapshot.currentFrame.has_value() &&
               event.snapshot.currentFrame->mode == core::client::PreviewPresentationMode::Interactive;
    });
    ASSERT_NE(events.cend(), interactiveFrame);
    EXPECT_FALSE(interactiveFrame->snapshot.currentFrame->analysis.has_value());
    const std::uint64_t interactiveSequence = interactiveFrame->snapshot.previewSequence.value;

    editorOrchestrator.endEdit();
    ASSERT_TRUE(waitForCondition([&events, interactiveSequence] {
        return std::any_of(events.cbegin(), events.cend(), [interactiveSequence](const auto& event) {
            return event.frameUpdated && event.snapshot.currentFrame.has_value() &&
                   event.snapshot.previewSequence.value > interactiveSequence &&
                   event.snapshot.currentFrame->mode == core::client::PreviewPresentationMode::Final &&
                   event.snapshot.currentFrame->analysis.has_value();
        });
    }));
}

TEST(QtPreviewPresentationEventAdapterTest, RemovesFramesAcrossResizeAndDevelopGenerations)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("generation.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("generation.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<PresentationPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    QtPreviewPresentationEventAdapter adapter(editorOrchestrator);
    std::vector<core::client::PreviewPresentationEvent> events;
    const core::client::PreviewPresentationSubscriptionResult subscription = adapter.subscribeToPreviewPresentation(
        [&events](const core::client::PreviewPresentationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    const core::catalog::CatalogEntry entry{
        {sourcePath,
         QStringLiteral("jpg"),
         QStringLiteral("generation.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(editorOrchestrator.activatePhoto(entry, QSize{640, 480}).hasValue());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(events.cbegin(), events.cend(), [](const auto& event) {
            return event.frameUpdated && event.snapshot.currentFrame.has_value();
        });
    }));
    const auto initialFrame = std::find_if(events.crbegin(), events.crend(), [](const auto& event) {
        return event.frameUpdated && event.snapshot.currentFrame.has_value();
    });
    ASSERT_NE(events.crend(), initialFrame);
    const std::uint64_t initialSequence = initialFrame->snapshot.previewSequence.value;
    const std::uint64_t initialRevision = initialFrame->snapshot.developRevision.value;

    ASSERT_TRUE(editorOrchestrator.updatePreviewTargetSize(QSize{800, 600}));
    ASSERT_TRUE(waitForCondition([&events, initialSequence, initialRevision] {
        return std::any_of(events.cbegin(), events.cend(), [initialSequence, initialRevision](const auto& event) {
            return event.snapshot.previewSequence.value > initialSequence &&
                   event.snapshot.developRevision.value == initialRevision && !event.snapshot.currentFrame.has_value();
        });
    }));
    ASSERT_TRUE(waitForCondition([&events, initialSequence] {
        return std::any_of(events.cbegin(), events.cend(), [initialSequence](const auto& event) {
            return event.frameUpdated && event.snapshot.currentFrame.has_value() &&
                   event.snapshot.previewSequence.value > initialSequence;
        });
    }));
    const auto resizedFrame = std::find_if(events.crbegin(), events.crend(), [initialSequence](const auto& event) {
        return event.frameUpdated && event.snapshot.currentFrame.has_value() &&
               event.snapshot.previewSequence.value > initialSequence;
    });
    ASSERT_NE(events.crend(), resizedFrame);
    const std::uint64_t resizedSequence = resizedFrame->snapshot.previewSequence.value;
    const std::uint64_t resizedRevision = resizedFrame->snapshot.developRevision.value;

    core::types::DevelopParams params;
    params.exposureEv = 0.5F;
    ASSERT_TRUE(editorOrchestrator.updateDevelopParams(params, QSize{800, 600}));
    ASSERT_TRUE(waitForCondition([&events, resizedSequence, resizedRevision] {
        return std::any_of(events.cbegin(), events.cend(), [resizedSequence, resizedRevision](const auto& event) {
            return event.snapshot.previewSequence.value > resizedSequence &&
                   event.snapshot.developRevision.value > resizedRevision && !event.snapshot.currentFrame.has_value();
        });
    }));
    ASSERT_TRUE(waitForCondition([&events, resizedSequence, resizedRevision] {
        return std::any_of(events.cbegin(), events.cend(), [resizedSequence, resizedRevision](const auto& event) {
            return event.frameUpdated && event.snapshot.currentFrame.has_value() &&
                   event.snapshot.previewSequence.value > resizedSequence &&
                   event.snapshot.developRevision.value > resizedRevision;
        });
    }));

    for (const core::client::PreviewPresentationEvent& event : events)
    {
        if (event.snapshot.currentFrame.has_value())
        {
            ASSERT_TRUE(event.snapshot.selectedPhotoId.has_value());
            EXPECT_EQ(event.snapshot.currentFrame->photoId, *event.snapshot.selectedPhotoId);
            EXPECT_EQ(event.snapshot.currentFrame->developRevision, event.snapshot.developRevision);
            EXPECT_EQ(event.snapshot.currentFrame->previewSequence, event.snapshot.previewSequence);
        }
    }
}

TEST(QtPreviewPresentationEventAdapterTest, CancelsExactlyOnceAndKeepsSameGenerationFrame)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("cancel.cr3"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("cancel.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    const auto probe = std::make_shared<ProgressiveCancellationProbe>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(
        std::make_unique<ProgressiveCancellationPipeline>(probe));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    QtPreviewPresentationEventAdapter adapter(editorOrchestrator);
    std::vector<core::client::PreviewPresentationEvent> events;
    const core::client::PreviewPresentationSubscriptionResult subscription = adapter.subscribeToPreviewPresentation(
        [&events](const core::client::PreviewPresentationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    const core::catalog::CatalogEntry entry{
        {sourcePath, QStringLiteral("cr3"), QStringLiteral("cancel.cr3"), core::types::SupportedFileKind::Raw},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(editorOrchestrator.activatePhoto(entry, QSize{640, 480}).hasValue());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(events.cbegin(), events.cend(), [](const auto& event) {
            return event.frameUpdated && event.snapshot.currentFrame.has_value();
        });
    }));
    ASSERT_TRUE(probe->standardStarted.tryAcquire(1, 5000));
    const auto frame = std::find_if(events.crbegin(), events.crend(), [](const auto& event) {
        return event.frameUpdated && event.snapshot.currentFrame.has_value();
    });
    ASSERT_NE(events.crend(), frame);
    ASSERT_TRUE(frame->snapshot.activeRequestId.has_value());
    const core::client::PreviewRequestId requestId = *frame->snapshot.activeRequestId;
    const core::client::PreviewSequence generation = frame->snapshot.previewSequence;

    core::client::IPreviewPresentationClient& previewClient = editorOrchestrator;
    ASSERT_TRUE(previewClient.cancelPreviewRequest(requestId).hasValue());
    ASSERT_TRUE(probe->cancellationObserved.tryAcquire(1, 5000));
    ASSERT_TRUE(waitForCondition([&events, requestId] {
        return std::any_of(events.cbegin(), events.cend(), [requestId](const auto& event) {
            return event.terminal.has_value() && event.terminal->requestId == requestId &&
                   event.terminal->state == core::client::PreviewTerminalState::Cancelled;
        });
    }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    const auto cancelledCount = std::count_if(events.cbegin(), events.cend(), [requestId](const auto& event) {
        return event.terminal.has_value() && event.terminal->requestId == requestId &&
               event.terminal->state == core::client::PreviewTerminalState::Cancelled;
    });
    const auto unexpectedTerminalCount = std::count_if(events.cbegin(), events.cend(), [requestId](const auto& event) {
        return event.terminal.has_value() && event.terminal->requestId == requestId &&
               event.terminal->state != core::client::PreviewTerminalState::Cancelled;
    });
    const auto cancelledEvent = std::find_if(events.crbegin(), events.crend(), [requestId](const auto& event) {
        return event.terminal.has_value() && event.terminal->requestId == requestId;
    });
    ASSERT_NE(events.crend(), cancelledEvent);
    EXPECT_EQ(1, cancelledCount);
    EXPECT_EQ(0, unexpectedTerminalCount);
    EXPECT_EQ(1, std::count_if(events.cbegin(), events.cend(), [](const auto& event) { return event.frameUpdated; }));
    EXPECT_TRUE(
        std::none_of(events.cbegin(), events.cend(), [](const auto& event) { return event.warning.has_value(); }));
    EXPECT_FALSE(cancelledEvent->snapshot.activeRequestId.has_value());
    ASSERT_TRUE(cancelledEvent->snapshot.currentFrame.has_value());
    EXPECT_EQ(generation, cancelledEvent->snapshot.previewSequence);
    EXPECT_EQ(generation, cancelledEvent->snapshot.currentFrame->previewSequence);
    EXPECT_FALSE(probe->timedOut.load(std::memory_order_acquire));
}

TEST(QtPreviewPresentationEventAdapterTest, KeepsValidFrameWhenLaterTierProjectionFails)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("projection.cr3"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("projection.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    core::orchestration::PreviewOrchestrator previewOrchestrator(
        std::make_unique<ProjectionFailureAfterFramePipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    QtPreviewPresentationEventAdapter adapter(editorOrchestrator);
    std::vector<core::client::PreviewPresentationEvent> events;
    const core::client::PreviewPresentationSubscriptionResult subscription = adapter.subscribeToPreviewPresentation(
        [&events](const core::client::PreviewPresentationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    const core::catalog::CatalogEntry entry{
        {sourcePath, QStringLiteral("cr3"), QStringLiteral("projection.cr3"), core::types::SupportedFileKind::Raw},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(editorOrchestrator.activatePhoto(entry, QSize{640, 480}).hasValue());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(events.cbegin(), events.cend(), [](const auto& event) {
            return event.terminal.has_value() && event.terminal->state == core::client::PreviewTerminalState::Completed;
        });
    }));

    EXPECT_EQ(1, std::count_if(events.cbegin(), events.cend(), [](const auto& event) { return event.frameUpdated; }));
    EXPECT_EQ(1, std::count_if(events.cbegin(), events.cend(), [](const auto& event) {
                  return event.warning.has_value() &&
                         event.warning->error.code == core::client::ClientErrorCode::Unknown;
              }));
    EXPECT_EQ(1, std::count_if(events.cbegin(), events.cend(), [](const auto& event) {
                  return event.terminal.has_value() &&
                         event.terminal->state == core::client::PreviewTerminalState::Completed;
              }));
    EXPECT_EQ(0, std::count_if(events.cbegin(), events.cend(), [](const auto& event) {
                  return event.terminal.has_value() &&
                         event.terminal->state == core::client::PreviewTerminalState::Failed;
              }));
    const auto terminal =
        std::find_if(events.crbegin(), events.crend(), [](const auto& event) { return event.terminal.has_value(); });
    ASSERT_NE(events.crend(), terminal);
    ASSERT_TRUE(terminal->snapshot.currentFrame.has_value());
    EXPECT_EQ(core::client::PreviewFrameTier::Thumbnail, terminal->snapshot.currentFrame->tier);
    EXPECT_EQ(terminal->snapshot.previewSequence, terminal->snapshot.currentFrame->previewSequence);
}

TEST(QtPreviewPresentationEventAdapterTest, UnsubscribeAndDestructionSuppressQueuedCallbacks)
{
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<PresentationPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    auto adapter = std::make_unique<QtPreviewPresentationEventAdapter>(editorOrchestrator);
    std::vector<core::client::PreviewPresentationEvent> events;
    const core::client::PreviewPresentationSubscriptionResult subscription = adapter->subscribeToPreviewPresentation(
        [&events](const core::client::PreviewPresentationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    core::client::PreviewPresentationSubscriptionHandle handle = subscription.value();

    handle->unsubscribe();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(handle->isActive());

    const core::client::PreviewPresentationSubscriptionResult outliving = adapter->subscribeToPreviewPresentation(
        [&events](const core::client::PreviewPresentationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(outliving.hasValue());
    core::client::PreviewPresentationSubscriptionHandle outlivingHandle = outliving.value();
    adapter.reset();
    EXPECT_FALSE(outlivingHandle->isActive());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
}

}  // namespace
}  // namespace flexraw::ui::facade

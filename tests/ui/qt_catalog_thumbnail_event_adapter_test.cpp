#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "qt_catalog_thumbnail_event_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

class DeterministicCatalogThumbnailPipeline final : public core::orchestration::ICatalogThumbnailPipeline
{
public:
    // 목적: adapter test에서 known frame 또는 item-level 오류를 deterministic하게 생성
    // 입력: source: display name으로 성공·실패 선택, targetSize: 미사용, cancellationToken: 취소 상태
    // 출력: 2x1 known-color image 또는 ThumbnailUnavailable 오류
    [[nodiscard]] core::orchestration::CatalogThumbnailPipelineResult load(
        const core::types::FileDescriptor& source,
        const QSize&,
        const core::types::CancellationToken& cancellationToken) override
    {
        if (cancellationToken.isCancellationRequested())
        {
            return core::orchestration::CatalogThumbnailPipelineResult::failure(
                {core::types::ErrorCode::Cancelled, QStringLiteral("Thumbnail adapter test request was cancelled.")});
        }
        if (source.displayName == QStringLiteral("failure.jpg"))
        {
            return core::orchestration::CatalogThumbnailPipelineResult::failure(
                {core::types::ErrorCode::ThumbnailUnavailable, QStringLiteral("Thumbnail adapter test failure.")});
        }

        QImage image(2, 1, QImage::Format_RGBA8888);
        image.setPixelColor(0, 0, QColor(10, 20, 30, 40));
        image.setPixelColor(1, 0, QColor(50, 60, 70, 80));
        return core::orchestration::CatalogThumbnailPipelineResult::success(std::move(image));
    }
};

// 목적: adapter test용 normalized absolute source locator를 가진 Qt-free item 생성
// 입력: displayName: pipeline 결과 선택, photoId: tagged Catalog identity
// 출력: owner command validation을 통과하는 raster thumbnail item
[[nodiscard]] core::client::CatalogThumbnailItem makeItem(const QString& displayName, std::int64_t photoId)
{
    const QString path =
        QDir::cleanPath(QDir(QDir::tempPath()).filePath(QStringLiteral("thumbnail-adapter/") + displayName));
    const QByteArray pathUtf8 = path.toUtf8();
    const QByteArray nameUtf8 = displayName.toUtf8();
    return {{core::client::CatalogThumbnailIdentityKind::CatalogPhoto, {photoId}, {}},
            {pathUtf8.constData(), static_cast<std::size_t>(pathUtf8.size())},
            "jpg",
            {nameUtf8.constData(), static_cast<std::size_t>(nameUtf8.size())},
            core::client::CatalogFileKind::RasterImage};
}

// 목적: Qt event를 처리하며 Catalog thumbnail event predicate를 bounded wait
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

TEST(QtCatalogThumbnailEventAdapterTest, PublishesTaggedFrameAndExactCompletedTerminal)
{
    core::orchestration::CatalogThumbnailOrchestrator orchestrator(
        std::make_unique<DeterministicCatalogThumbnailPipeline>());
    QtCatalogThumbnailEventAdapter adapter(orchestrator);
    std::vector<core::client::CatalogThumbnailEvent> events;
    bool subscribeReturned = false;
    bool reenteredSubscribe = false;
    const core::client::CatalogThumbnailSubscriptionResult subscription = adapter.subscribeToCatalogThumbnails(
        [&events, &subscribeReturned, &reenteredSubscribe](const core::client::CatalogThumbnailEvent& event) {
            reenteredSubscribe = reenteredSubscribe || !subscribeReturned;
            events.push_back(event);
        });
    subscribeReturned = true;
    ASSERT_TRUE(subscription.hasValue());
    const core::client::CatalogThumbnailItem item = makeItem(QStringLiteral("success.jpg"), 37);

    const core::client::CatalogThumbnailWindowResult receipt = orchestrator.replaceThumbnailWindow({{item}, {96, 72}});

    ASSERT_TRUE(receipt.hasValue());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(events.cbegin(), events.cend(), [](const auto& event) {
            return event.terminal.has_value() &&
                   event.terminal->state == core::client::CatalogThumbnailTerminalState::Completed;
        });
    }));
    EXPECT_FALSE(reenteredSubscribe);
    ASSERT_FALSE(events.empty());
    EXPECT_TRUE(events.front().initial);
    const auto started =
        std::find_if(events.cbegin(), events.cend(), [](const auto& event) { return event.windowStarted; });
    const auto frame =
        std::find_if(events.cbegin(), events.cend(), [](const auto& event) { return event.frame.has_value(); });
    const auto terminal = std::find_if(events.cbegin(), events.cend(), [](const auto& event) {
        return event.terminal.has_value() &&
               event.terminal->state == core::client::CatalogThumbnailTerminalState::Completed;
    });
    ASSERT_NE(events.cend(), started);
    ASSERT_NE(events.cend(), frame);
    ASSERT_NE(events.cend(), terminal);
    EXPECT_EQ(receipt.value().generation, started->snapshot.activeGeneration);
    ASSERT_TRUE(frame->frame.has_value());
    EXPECT_EQ(receipt.value().generation, frame->frame->generation);
    EXPECT_EQ(item.identity, frame->frame->identity);
    EXPECT_EQ(2U, frame->frame->frame.width());
    EXPECT_EQ(1U, frame->frame->frame.height());
    ASSERT_EQ(8U, frame->frame->frame.bytes().size());
    EXPECT_EQ(30U, frame->frame->frame.bytes()[0]);
    EXPECT_EQ(20U, frame->frame->frame.bytes()[1]);
    EXPECT_EQ(10U, frame->frame->frame.bytes()[2]);
    EXPECT_EQ(255U, frame->frame->frame.bytes()[3]);
    EXPECT_EQ(receipt.value().generation.value, frame->frame->frame.previewRevision());
    EXPECT_EQ(receipt.value().generation, terminal->terminal->generation);
    EXPECT_FALSE(terminal->snapshot.activeGeneration.has_value());
    EXPECT_LT(started->sequence.value, frame->sequence.value);
    EXPECT_LT(frame->sequence.value, terminal->sequence.value);
}

TEST(QtCatalogThumbnailEventAdapterTest, MapsItemIssueWithoutChangingWindowCompletion)
{
    core::orchestration::CatalogThumbnailOrchestrator orchestrator(
        std::make_unique<DeterministicCatalogThumbnailPipeline>());
    QtCatalogThumbnailEventAdapter adapter(orchestrator);
    std::vector<core::client::CatalogThumbnailEvent> events;
    const core::client::CatalogThumbnailSubscriptionResult subscription = adapter.subscribeToCatalogThumbnails(
        [&events](const core::client::CatalogThumbnailEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    const core::client::CatalogThumbnailItem item = makeItem(QStringLiteral("failure.jpg"), 41);

    const core::client::CatalogThumbnailWindowResult receipt = orchestrator.replaceThumbnailWindow({{item}, {96, 72}});

    ASSERT_TRUE(receipt.hasValue());
    ASSERT_TRUE(waitForCondition([&events] {
        return std::any_of(
            events.cbegin(), events.cend(), [](const auto& event) { return event.terminal.has_value(); });
    }));
    const auto issue =
        std::find_if(events.cbegin(), events.cend(), [](const auto& event) { return event.issue.has_value(); });
    const auto terminal =
        std::find_if(events.cbegin(), events.cend(), [](const auto& event) { return event.terminal.has_value(); });
    ASSERT_NE(events.cend(), issue);
    ASSERT_NE(events.cend(), terminal);
    EXPECT_EQ(item.identity, issue->issue->identity);
    EXPECT_EQ(core::client::ClientErrorCode::ThumbnailUnavailable, issue->issue->error.code);
    EXPECT_EQ(core::client::CatalogThumbnailTerminalState::Completed, terminal->terminal->state);
    EXPECT_FALSE(terminal->terminal->error.has_value());
}

TEST(QtCatalogThumbnailEventAdapterTest, UnsubscribeAndDestructionSuppressQueuedCallbacks)
{
    core::orchestration::CatalogThumbnailOrchestrator orchestrator(
        std::make_unique<DeterministicCatalogThumbnailPipeline>());
    auto adapter = std::make_unique<QtCatalogThumbnailEventAdapter>(orchestrator);
    std::vector<core::client::CatalogThumbnailEvent> events;
    const core::client::CatalogThumbnailSubscriptionResult subscription = adapter->subscribeToCatalogThumbnails(
        [&events](const core::client::CatalogThumbnailEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    core::client::CatalogThumbnailSubscriptionHandle handle = subscription.value();

    handle->unsubscribe();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(handle->isActive());

    const core::client::CatalogThumbnailSubscriptionResult outliving = adapter->subscribeToCatalogThumbnails(
        [&events](const core::client::CatalogThumbnailEvent& event) { events.push_back(event); });
    ASSERT_TRUE(outliving.hasValue());
    core::client::CatalogThumbnailSubscriptionHandle outlivingHandle = outliving.value();
    adapter.reset();
    EXPECT_FALSE(outlivingHandle->isActive());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
}

}  // namespace
}  // namespace flexraw::ui::facade

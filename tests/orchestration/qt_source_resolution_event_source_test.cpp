#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "qt_source_resolution_event_source.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

// 목적: Source Resolution adapter test용 source file 생성
// 입력: path: 생성할 file path, contents: fingerprint 대상 bytes
// 출력: 전체 file 기록 성공 여부
[[nodiscard]] bool writeSourceFile(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
           file.flush();
}

// 목적: test source를 Catalog import용 Ready entry로 조립
// 입력: sourcePath: absolute source path
// 출력: raster file metadata를 가진 Catalog entry
[[nodiscard]] core::catalog::CatalogEntry makeEntry(const QString& sourcePath)
{
    const QFileInfo sourceInfo(sourcePath);
    return {core::types::makeFileDescriptor(sourceInfo, core::types::SupportedFileKind::RasterImage),
            core::types::FileScanStatus::Ready};
}

// 목적: Qt delivery를 처리하며 Source Resolution event predicate를 bounded wait
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

// 목적: event 목록에서 지정 request의 exact terminal 존재 여부 확인
// 입력: events: delivered source events, requestId: owner request identity
// 출력: matching terminal을 찾으면 true
[[nodiscard]] bool hasTerminal(const std::vector<core::client::SourceResolutionEvent>& events,
                               core::client::SourceRequestId requestId)
{
    return std::any_of(events.cbegin(), events.cend(), [requestId](const auto& event) {
        return event.terminal.has_value() && event.terminal->receipt.id == requestId;
    });
}

TEST(QtSourceResolutionEventSourceTest, PublishesBaselineAcceptedUpdateAndExactCompletedTerminal)
{
    static_cast<void>(test::application());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("baseline.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("baseline-source")));
    core::orchestration::CatalogOrchestrator orchestrator;
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    QtSourceResolutionEventSource adapter(orchestrator);
    std::vector<core::client::SourceResolutionEvent> events;
    bool subscribeReturned = false;
    bool reenteredSubscribe = false;
    QThread* callbackThread = nullptr;
    const core::client::SourceResolutionSubscriptionResult subscription =
        adapter.subscribeToSourceResolution([&](const core::client::SourceResolutionEvent& event) {
            reenteredSubscribe = reenteredSubscribe || !subscribeReturned;
            callbackThread = QThread::currentThread();
            events.push_back(event);
        });
    ASSERT_TRUE(subscription.hasValue());
    subscribeReturned = true;
    ASSERT_TRUE(waitForCondition([&events] { return !events.empty(); }));

    const core::orchestration::CatalogImportResult imported =
        orchestrator.importScannedEntries({makeEntry(sourcePath)});

    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().fingerprintRequestIds.size());
    const core::client::SourceRequestId requestId{imported.value().fingerprintRequestIds.front()};
    ASSERT_TRUE(waitForCondition([&events, requestId] { return hasTerminal(events, requestId); }));
    EXPECT_FALSE(reenteredSubscribe);
    EXPECT_EQ(adapter.thread(), callbackThread);
    ASSERT_TRUE(events.front().initial);
    EXPECT_TRUE(events.front().snapshot.activeRequests.empty());
    const auto accepted = std::find_if(events.cbegin(), events.cend(), [requestId](const auto& event) {
        return event.accepted.has_value() && event.accepted->id == requestId;
    });
    const auto completed = std::find_if(events.cbegin(), events.cend(), [requestId](const auto& event) {
        return event.terminal.has_value() && event.terminal->receipt.id == requestId;
    });
    ASSERT_NE(events.cend(), accepted);
    ASSERT_NE(events.cend(), completed);
    EXPECT_EQ(core::client::SourceRequestKind::EstablishBaseline, accepted->accepted->kind);
    EXPECT_EQ(imported.value().photoIds.front().value, accepted->accepted->photoId.value);
    EXPECT_EQ(sourcePath.toUtf8().toStdString(), accepted->accepted->sourceLocator);
    ASSERT_TRUE(completed->update.has_value());
    EXPECT_EQ(core::client::CatalogSourceState::Available, completed->update->photo.sourceState);
    EXPECT_EQ(core::types::Sha256DigestSize,
              static_cast<qsizetype>(completed->update->photo.fingerprint.sha256.size()));
    EXPECT_EQ(core::client::SourceResolutionTerminalState::Completed, completed->terminal->state);
    EXPECT_FALSE(completed->terminal->error.has_value());
    EXPECT_TRUE(completed->snapshot.activeRequests.empty());
    EXPECT_LT(accepted->sequence.value, completed->sequence.value);
}

TEST(QtSourceResolutionEventSourceTest, CancellationUsesNeutralClientAndPublishesOneCancelledTerminal)
{
    static_cast<void>(test::application());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("cancel.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("cancel-source")));
    core::orchestration::CatalogOrchestrator orchestrator;
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    QtSourceResolutionEventSource adapter(orchestrator);
    std::vector<core::client::SourceResolutionEvent> events;
    const core::client::SourceResolutionSubscriptionResult subscription = adapter.subscribeToSourceResolution(
        [&events](const core::client::SourceResolutionEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForCondition([&events] { return !events.empty(); }));
    events.clear();
    const core::orchestration::CatalogImportResult imported =
        orchestrator.importScannedEntries({makeEntry(sourcePath)});
    ASSERT_TRUE(imported.hasValue());
    const core::client::SourceRequestId requestId{imported.value().fingerprintRequestIds.front()};

    const core::client::SourceRequestCancelResult cancelled = orchestrator.cancelSourceRequest(requestId);

    ASSERT_TRUE(cancelled.hasValue());
    ASSERT_TRUE(waitForCondition([&events, requestId] { return hasTerminal(events, requestId); }));
    const auto terminal = std::find_if(events.cbegin(), events.cend(), [requestId](const auto& event) {
        return event.terminal.has_value() && event.terminal->receipt.id == requestId;
    });
    ASSERT_NE(events.cend(), terminal);
    EXPECT_EQ(core::client::SourceResolutionTerminalState::Cancelled, terminal->terminal->state);
    EXPECT_TRUE(terminal->snapshot.activeRequests.empty());
    EXPECT_EQ(1, std::count_if(events.cbegin(), events.cend(), [requestId](const auto& event) {
                  return event.terminal.has_value() && event.terminal->receipt.id == requestId;
              }));
}

TEST(QtSourceResolutionEventSourceTest, RelinkFailurePreservesIssueAndFailedTerminalContext)
{
    static_cast<void>(test::application());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("original.jpg"));
    const QString missingPath =
        QDir::cleanPath(QDir::fromNativeSeparators(QDir(directory.path()).filePath(QStringLiteral("missing.jpg"))));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("relink-source")));
    core::orchestration::CatalogOrchestrator orchestrator;
    ASSERT_TRUE(orchestrator.openCatalog(catalogPath).hasValue());
    int sourceUpdateCount = 0;
    QObject::connect(&orchestrator,
                     &core::orchestration::CatalogOrchestrator::sourceBindingUpdated,
                     [&sourceUpdateCount](const auto&) { ++sourceUpdateCount; });
    const core::orchestration::CatalogImportResult imported =
        orchestrator.importScannedEntries({makeEntry(sourcePath)});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_TRUE(waitForCondition([&sourceUpdateCount] { return sourceUpdateCount == 1; }));
    const core::types::PhotoId photoId = imported.value().photoIds.front();
    ASSERT_TRUE(QFile::remove(sourcePath));
    const core::orchestration::CatalogPhotoStateResult missing = orchestrator.resolvePhoto(photoId);
    ASSERT_TRUE(missing.hasValue());
    ASSERT_EQ(core::catalog::SourceBindingState::Missing, missing.value().photo.sourceState);
    QtSourceResolutionEventSource adapter(orchestrator);
    std::vector<core::client::SourceResolutionEvent> events;
    const core::client::SourceResolutionSubscriptionResult subscription = adapter.subscribeToSourceResolution(
        [&events](const core::client::SourceResolutionEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForCondition([&events] { return !events.empty(); }));
    events.clear();
    const core::client::SourceRequestResult relinked =
        orchestrator.relinkSource({{photoId.value}, missingPath.toUtf8().toStdString()});

    ASSERT_TRUE(relinked.hasValue());
    ASSERT_TRUE(
        waitForCondition([&events, requestId = relinked.value().id] { return hasTerminal(events, requestId); }));
    const auto failed = std::find_if(events.cbegin(), events.cend(), [&](const auto& event) {
        return event.terminal.has_value() && event.terminal->receipt.id == relinked.value().id;
    });
    ASSERT_NE(events.cend(), failed);
    ASSERT_TRUE(failed->issue.has_value());
    EXPECT_EQ(core::client::SourceRequestKind::RelinkSource, failed->issue->receipt.kind);
    EXPECT_EQ(missingPath.toUtf8().toStdString(), failed->issue->receipt.sourceLocator);
    EXPECT_EQ(core::client::ClientErrorCode::NotFound, failed->issue->error.code);
    EXPECT_EQ(core::client::SourceResolutionTerminalState::Failed, failed->terminal->state);
    ASSERT_TRUE(failed->terminal->error.has_value());
    EXPECT_EQ(failed->issue->error, *failed->terminal->error);
}

TEST(QtSourceResolutionEventSourceTest, RejectsInvalidIdentityAndNonNormalizedRelinkLocatorBeforeAcceptance)
{
    static_cast<void>(test::application());
    core::orchestration::CatalogOrchestrator orchestrator;
    const core::client::SourceRequestResult invalidIdentity =
        orchestrator.acceptReplacement(core::client::AcceptReplacementCommand{{0}});
    const QString nonNormalized =
        QDir::fromNativeSeparators(QDir(QDir::tempPath()).filePath(QStringLiteral("source-contract/../photo.jpg")));
    const core::client::SourceRequestResult nonNormalizedResult =
        orchestrator.relinkSource({{1}, nonNormalized.toUtf8().toStdString()});

    ASSERT_TRUE(invalidIdentity.hasError());
    EXPECT_EQ(core::client::ClientErrorCode::InvalidArgument, invalidIdentity.error().code);
    ASSERT_TRUE(nonNormalizedResult.hasError());
    EXPECT_EQ(core::client::ClientErrorCode::InvalidArgument, nonNormalizedResult.error().code);
    EXPECT_TRUE(orchestrator.sourceResolutionSnapshot().activeRequests.empty());
}

TEST(QtSourceResolutionEventSourceTest, UnsubscribeAndAdapterDestructionSuppressQueuedCallbacks)
{
    static_cast<void>(test::application());
    core::orchestration::CatalogOrchestrator orchestrator;
    auto adapter = std::make_unique<QtSourceResolutionEventSource>(orchestrator);
    std::vector<core::client::SourceResolutionEvent> events;
    const core::client::SourceResolutionSubscriptionResult subscription = adapter->subscribeToSourceResolution(
        [&events](const core::client::SourceResolutionEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    core::client::SourceResolutionSubscriptionHandle handle = subscription.value();

    handle->unsubscribe();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(handle->isActive());

    const core::client::SourceResolutionSubscriptionResult outliving = adapter->subscribeToSourceResolution(
        [&events](const core::client::SourceResolutionEvent& event) { events.push_back(event); });
    ASSERT_TRUE(outliving.hasValue());
    core::client::SourceResolutionSubscriptionHandle outlivingHandle = outliving.value();
    adapter.reset();
    EXPECT_FALSE(outlivingHandle->isActive());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
}

}  // namespace
}  // namespace flexraw::core::orchestration

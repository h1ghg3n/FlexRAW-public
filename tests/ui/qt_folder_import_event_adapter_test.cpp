#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_orchestrator.h"
#include "qt_folder_import_event_adapter.h"

namespace flexraw::ui::facade
{
namespace
{

// 목적: Folder event adapter test용 source file 생성
// 입력: path: 생성할 file path, contents: 기록할 bytes
// 출력: 전체 file 기록 성공 여부
[[nodiscard]] bool writeSourceFile(const QString& path, const QByteArray& contents = QByteArray("folder-event"))
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
           file.flush();
}

// 목적: Qt delivery context를 처리하며 Folder event 수가 목표에 도달할 때까지 bounded wait
// 입력: events: callback 누적 event, expectedCount: 목표 수, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 목표 event를 받으면 true
[[nodiscard]] bool waitForFolderEvents(const std::vector<core::client::FolderOperationEvent>& events,
                                       std::size_t expectedCount,
                                       int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs)
    {
        if (events.size() >= expectedCount)
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return events.size() >= expectedCount;
}

TEST(QtFolderImportEventAdapterTest, DeliversInitialAcceptedAndExactScanTerminalAsynchronously)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeSourceFile(QDir(directory.path()).filePath(QStringLiteral("photo.jpg"))));
    ASSERT_TRUE(writeSourceFile(QDir(directory.path()).filePath(QStringLiteral("ignored.txt"))));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    QtFolderImportEventAdapter adapter(catalogOrchestrator);
    std::vector<core::client::FolderOperationEvent> events;
    bool subscribeReturned = false;
    bool reenteredSubscribe = false;
    QThread* callbackThread = nullptr;
    const core::client::FolderOperationSubscriptionResult subscription =
        adapter.subscribeToFolderOperations([&](const core::client::FolderOperationEvent& event) {
            reenteredSubscribe = !subscribeReturned;
            callbackThread = QThread::currentThread();
            events.push_back(event);
        });
    ASSERT_TRUE(subscription.hasValue());
    EXPECT_TRUE(events.empty());
    subscribeReturned = true;
    ASSERT_TRUE(waitForFolderEvents(events, 1));

    const core::client::FolderOperationResult submitted =
        catalogOrchestrator.submitFolderScan({directory.path().toUtf8().toStdString()});
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForFolderEvents(events, 3));

    EXPECT_FALSE(reenteredSubscribe);
    EXPECT_EQ(adapter.thread(), callbackThread);
    ASSERT_EQ(3U, events.size());
    EXPECT_TRUE(events[0].initial);
    EXPECT_FALSE(events[0].activeOperation.has_value());
    ASSERT_TRUE(events[1].activeOperation.has_value());
    EXPECT_EQ(submitted.value(), *events[1].activeOperation);
    EXPECT_FALSE(events[1].terminal.has_value());
    EXPECT_FALSE(events[2].activeOperation.has_value());
    ASSERT_TRUE(events[2].terminal.has_value());
    EXPECT_EQ(submitted.value(), events[2].terminal->receipt);
    EXPECT_EQ(core::client::FolderOperationTerminalState::Completed, events[2].terminal->state);
    ASSERT_TRUE(events[2].terminal->completion.has_value());
    ASSERT_TRUE(std::holds_alternative<core::client::FolderScanCompletion>(*events[2].terminal->completion));
    const core::client::FolderScanCompletion& completion =
        std::get<core::client::FolderScanCompletion>(*events[2].terminal->completion);
    ASSERT_EQ(1U, completion.items.size());
    EXPECT_EQ("photo.jpg", completion.items.front().displayName);
    EXPECT_LT(events[0].sequence.value, events[1].sequence.value);
    EXPECT_LT(events[1].sequence.value, events[2].sequence.value);
}

TEST(QtFolderImportEventAdapterTest, PublishesImportTerminalOnlyAfterCatalogPersistence)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeSourceFile(QDir(directory.path()).filePath(QStringLiteral("persisted.jpg"))));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    QtFolderImportEventAdapter adapter(catalogOrchestrator);
    std::vector<core::client::FolderOperationEvent> events;
    const core::client::FolderOperationSubscriptionResult subscription = adapter.subscribeToFolderOperations(
        [&](const core::client::FolderOperationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForFolderEvents(events, 1));

    const core::client::FolderOperationResult submitted = catalogOrchestrator.submitFolderImport(
        {directory.path().toUtf8().toStdString(), catalogOrchestrator.state().catalogPath.toUtf8().toStdString()});
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForFolderEvents(events, 3));

    ASSERT_TRUE(events.back().terminal.has_value());
    ASSERT_TRUE(events.back().terminal->completion.has_value());
    ASSERT_TRUE(std::holds_alternative<core::client::FolderImportCompletion>(*events.back().terminal->completion));
    const core::client::FolderImportCompletion& completion =
        std::get<core::client::FolderImportCompletion>(*events.back().terminal->completion);
    EXPECT_EQ(1, completion.discoveredCount);
    EXPECT_EQ(1, completion.appliedCount);
    ASSERT_EQ(1U, completion.photoIds.size());
    const core::client::CatalogPhotoPageResult photos = catalogOrchestrator.queryPhotoPage({});
    ASSERT_TRUE(photos.hasValue());
    ASSERT_EQ(1U, photos.value().photos.size());
    EXPECT_EQ(completion.photoIds.front(), photos.value().photos.front().id);
}

TEST(QtFolderImportEventAdapterTest, InitialSnapshotIncludesOperationAcceptedBeforeAdapterConstruction)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeSourceFile(QDir(directory.path()).filePath(QStringLiteral("already-active.jpg"))));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    const core::client::FolderOperationResult submitted =
        catalogOrchestrator.submitFolderScan({directory.path().toUtf8().toStdString()});
    ASSERT_TRUE(submitted.hasValue());
    QtFolderImportEventAdapter adapter(catalogOrchestrator);
    std::vector<core::client::FolderOperationEvent> events;

    const core::client::FolderOperationSubscriptionResult subscription = adapter.subscribeToFolderOperations(
        [&](const core::client::FolderOperationEvent& event) { events.push_back(event); });

    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForFolderEvents(events, 1));
    EXPECT_TRUE(events.front().initial);
    ASSERT_TRUE(events.front().activeOperation.has_value());
    EXPECT_EQ(submitted.value(), *events.front().activeOperation);
}

TEST(QtFolderImportEventAdapterTest, UnsubscribeAndAdapterDestructionSuppressQueuedCallbacks)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeSourceFile(QDir(directory.path()).filePath(QStringLiteral("photo.jpg"))));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    auto adapter = std::make_unique<QtFolderImportEventAdapter>(catalogOrchestrator);
    std::vector<core::client::FolderOperationEvent> events;
    const core::client::FolderOperationSubscriptionResult subscription = adapter->subscribeToFolderOperations(
        [&](const core::client::FolderOperationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    core::client::FolderOperationSubscriptionHandle handle = subscription.value();

    handle->unsubscribe();
    ASSERT_TRUE(catalogOrchestrator.submitFolderScan({directory.path().toUtf8().toStdString()}).hasValue());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(handle->isActive());

    const core::client::FolderOperationSubscriptionResult outliving = adapter->subscribeToFolderOperations(
        [&](const core::client::FolderOperationEvent& event) { events.push_back(event); });
    ASSERT_TRUE(outliving.hasValue());
    core::client::FolderOperationSubscriptionHandle outlivingHandle = outliving.value();
    adapter.reset();
    EXPECT_FALSE(outlivingHandle->isActive());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
}

}  // namespace
}  // namespace flexraw::ui::facade

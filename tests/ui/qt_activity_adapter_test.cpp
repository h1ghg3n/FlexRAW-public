#include <algorithm>
#include <memory>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QLabel>
#include <QProgressBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QToolButton>

#include <gtest/gtest.h>

#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "catalog_session_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "editor_orchestrator.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "mainwindow.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "qt_activity_adapter.h"
#include "qt_export_client_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_source_resolution_event_source.h"
#include "qt_worker_profile_settings_adapter.h"
#include "test_path_identity_service.h"

namespace flexraw::ui::mainwindow
{
namespace
{

class DormantActivityPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: Activity adapter test에서 실제 preview processing 생략
    // 입력: request/tier/cancellationToken: contract 일치를 위한 미사용 context
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Activity test does not render previews.")});
    }
};

class ActivityAdapterContext
{
public:
    // 목적: Activity adapter test용 Preview, Catalog, Editor, facade와 Folder owner graph 구성
    // 입력: 없음
    // 출력: adapter가 가장 먼저 파괴되는 deterministic test context
    ActivityAdapterContext()
        : previewOrchestrator(std::make_unique<DormantActivityPreviewPipeline>()),
          catalogThumbnailOrchestrator(std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>()),
          editorOrchestrator(previewOrchestrator, catalogOrchestrator),
          catalogSessionOrchestrator(catalogOrchestrator, editorOrchestrator),
          sourceResolutionEventSource(catalogOrchestrator),
          catalogEditorFacade(catalogOrchestrator,
                              catalogSessionOrchestrator,
                              catalogThumbnailOrchestrator,
                              editorOrchestrator,
                              sourceResolutionEventSource),
          activityAdapter(std::make_unique<QtActivityAdapter>(
              catalogEditorFacade, catalogEditorFacade, catalogEditorFacade, catalogEditorFacade, catalogEditorFacade))
    {}

    core::orchestration::PreviewOrchestrator previewOrchestrator;
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator;
    core::orchestration::EditorOrchestrator editorOrchestrator;
    core::orchestration::CatalogSessionOrchestrator catalogSessionOrchestrator;
    core::orchestration::QtSourceResolutionEventSource sourceResolutionEventSource;
    facade::CatalogEditorFacade catalogEditorFacade;
    std::unique_ptr<QtActivityAdapter> activityAdapter;
};

// 목적: Qt delivery context를 처리하며 Activity event 수가 목표에 도달할 때까지 bounded wait
// 입력: events: callback 누적 event, expectedCount: 목표 수, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 목표 event를 받으면 true
[[nodiscard]] bool waitForActivityEvents(const std::vector<core::client::ActivityEvent>& events,
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

// 목적: Qt delivery를 처리하며 지정 kind의 Activity terminal을 기다림
// 입력: events: callback 누적 event, kind: 찾을 Activity 종류, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 matching terminal을 받으면 true
[[nodiscard]] bool waitForActivityTerminal(const std::vector<core::client::ActivityEvent>& events,
                                           core::client::ActivityKind kind,
                                           int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();
    const auto hasTerminal = [&events, kind] {
        return std::any_of(events.cbegin(), events.cend(), [kind](const core::client::ActivityEvent& event) {
            return event.terminal.has_value() && event.terminal->id.kind == kind;
        });
    };
    while (timeout.elapsed() < timeoutMs && !hasTerminal())
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return hasTerminal();
}

// 목적: source verification Activity test용 file 생성
// 입력: path: 기록할 source path, contents: fingerprint bytes
// 출력: 전체 bytes 기록 성공 여부
[[nodiscard]] bool writeSourceFile(const QString& path, const QByteArray& contents = QByteArray("activity-source"))
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
           file.flush();
}

// 목적: Activity accepted event가 cancellation 전에 terminal과 경합하지 않도록 충분히 큰 source file 생성
// 입력: path: 기록할 source path
// 출력: 256 MiB 크기 설정과 flush 성공 여부
[[nodiscard]] bool writeLargeSourceFile(const QString& path)
{
    constexpr qint64 TestSourceSize = 256LL * 1024LL * 1024LL;
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.resize(TestSourceSize) && file.flush();
}

TEST(QtActivityAdapterTest, DeliversInitialEmptySnapshotAsynchronouslyOnQtContext)
{
    ActivityAdapterContext context;
    std::vector<core::client::ActivityEvent> events;
    bool subscribeReturned = false;
    bool reenteredSubscribe = false;
    QThread* callbackThread = nullptr;

    const core::client::ActivitySubscriptionResult subscription =
        context.activityAdapter->subscribeToActivities([&](const core::client::ActivityEvent& event) {
            reenteredSubscribe = !subscribeReturned;
            callbackThread = QThread::currentThread();
            events.push_back(event);
        });
    ASSERT_TRUE(subscription.hasValue());
    EXPECT_TRUE(events.empty());
    subscribeReturned = true;
    ASSERT_TRUE(waitForActivityEvents(events, 1));

    EXPECT_FALSE(reenteredSubscribe);
    EXPECT_EQ(context.activityAdapter->thread(), callbackThread);
    ASSERT_EQ(1U, events.size());
    EXPECT_TRUE(events.front().initial);
    EXPECT_TRUE(events.front().activeActivities.empty());
    EXPECT_FALSE(events.front().terminal.has_value());
    EXPECT_GT(events.front().sequence.value, 0U);
}

TEST(QtActivityAdapterTest, PreservesPreviewStartAndTerminalOrdering)
{
    ActivityAdapterContext context;
    std::vector<core::client::ActivityEvent> events;
    const core::client::ActivitySubscriptionResult subscription = context.activityAdapter->subscribeToActivities(
        [&events](const core::client::ActivityEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForActivityEvents(events, 1));
    events.clear();

    context.editorOrchestrator.previewStarted(41);
    context.editorOrchestrator.previewCompleted(41);
    EXPECT_TRUE(events.empty());
    ASSERT_TRUE(waitForActivityEvents(events, 2));

    ASSERT_EQ(2U, events.size());
    ASSERT_EQ(1U, events[0].activeActivities.size());
    EXPECT_EQ(core::client::ActivityId({core::client::ActivityKind::Preview, 41}),
              events[0].activeActivities.front().id);
    EXPECT_TRUE(events[0].activeActivities.front().canCancel);
    EXPECT_FALSE(events[0].terminal.has_value());
    EXPECT_TRUE(events[1].activeActivities.empty());
    ASSERT_TRUE(events[1].terminal.has_value());
    EXPECT_EQ(events[0].activeActivities.front().id, events[1].terminal->id);
    EXPECT_EQ(core::client::ActivityTerminalState::Completed, events[1].terminal->state);
    EXPECT_LT(events[0].sequence.value, events[1].sequence.value);
}

TEST(QtActivityAdapterTest, RoutesSourceCancellationAndPublishesTerminalEvent)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("source.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(writeLargeSourceFile(sourcePath));
    ActivityAdapterContext context;
    std::vector<core::client::ActivityEvent> events;
    const core::client::ActivitySubscriptionResult subscription = context.activityAdapter->subscribeToActivities(
        [&events](const core::client::ActivityEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForActivityEvents(events, 1));
    events.clear();
    ASSERT_TRUE(context.catalogEditorFacade
                    .openCatalog({catalogPath.toUtf8().toStdString(),
                                  core::client::CatalogOpenMode::CreateNew,
                                  core::client::CatalogReplacementPolicy::Reject})
                    .hasValue());
    const core::catalog::CatalogEntry entry{
        {sourcePath, QStringLiteral("jpg"), QStringLiteral("source.jpg"), core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };
    const core::orchestration::CatalogImportResult imported = context.catalogOrchestrator.importScannedEntries({entry});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().fingerprintRequestIds.size());
    const core::client::ActivityId activityId{
        core::client::ActivityKind::SourceVerification,
        imported.value().fingerprintRequestIds.front(),
    };
    ASSERT_TRUE(waitForActivityEvents(events, 1));

    const core::client::ActivityCancelResult cancelled = context.activityAdapter->cancelActivity(activityId);

    ASSERT_TRUE(cancelled.hasValue());
    EXPECT_EQ(activityId, cancelled.value());
    ASSERT_TRUE(waitForActivityEvents(events, 2));
    ASSERT_EQ(2U, events.size());
    ASSERT_EQ(1U, events[0].activeActivities.size());
    EXPECT_EQ(activityId, events[0].activeActivities.front().id);
    EXPECT_TRUE(events[0].activeActivities.front().canCancel);
    EXPECT_TRUE(events[1].activeActivities.empty());
    ASSERT_TRUE(events[1].terminal.has_value());
    EXPECT_EQ(activityId, events[1].terminal->id);
    EXPECT_EQ(core::client::ActivityTerminalState::Cancelled, events[1].terminal->state);
}

TEST(QtActivityAdapterTest, ProjectsFolderScanAsNonCancellableActivity)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeSourceFile(QDir(directory.path()).filePath(QStringLiteral("photo.jpg"))));
    ActivityAdapterContext context;
    std::vector<core::client::ActivityEvent> events;
    const core::client::ActivitySubscriptionResult subscription = context.activityAdapter->subscribeToActivities(
        [&events](const core::client::ActivityEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForActivityEvents(events, 1));
    events.clear();

    const core::client::FolderOperationResult submitted =
        context.catalogEditorFacade.submitFolderScan({directory.path().toUtf8().toStdString()});
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForActivityEvents(events, 2));

    ASSERT_EQ(2U, events.size());
    ASSERT_EQ(1U, events[0].activeActivities.size());
    const core::client::ActiveActivity& started = events[0].activeActivities.front();
    EXPECT_EQ(core::client::ActivityKind::FolderScan, started.id.kind);
    EXPECT_GT(started.id.value, 0U);
    EXPECT_FALSE(started.canCancel);
    EXPECT_TRUE(events[1].activeActivities.empty());
    ASSERT_TRUE(events[1].terminal.has_value());
    EXPECT_EQ(started.id, events[1].terminal->id);
    EXPECT_EQ(core::client::ActivityTerminalState::Completed, events[1].terminal->state);
}

TEST(QtActivityAdapterTest, ProjectsFolderImportTerminalAfterCatalogPersistence)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(writeSourceFile(QDir(directory.path()).filePath(QStringLiteral("imported.jpg"))));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ActivityAdapterContext context;
    ASSERT_TRUE(context.catalogEditorFacade
                    .openCatalog({catalogPath.toUtf8().toStdString(),
                                  core::client::CatalogOpenMode::CreateNew,
                                  core::client::CatalogReplacementPolicy::Reject})
                    .hasValue());
    std::vector<core::client::ActivityEvent> events;
    const core::client::ActivitySubscriptionResult subscription = context.activityAdapter->subscribeToActivities(
        [&events](const core::client::ActivityEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForActivityEvents(events, 1));
    events.clear();

    const core::client::FolderOperationResult submitted = context.catalogEditorFacade.submitFolderImport(
        {directory.path().toUtf8().toStdString(), context.catalogEditorFacade.catalogSnapshot().catalogPath});
    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForActivityTerminal(events, core::client::ActivityKind::FolderImport));

    const core::client::ActivityId expectedId{core::client::ActivityKind::FolderImport, submitted.value().id.value};
    const auto started = std::find_if(events.cbegin(), events.cend(), [&expectedId](const auto& event) {
        return std::any_of(event.activeActivities.cbegin(),
                           event.activeActivities.cend(),
                           [&expectedId](const auto& activity) { return activity.id == expectedId; });
    });
    const auto terminal = std::find_if(events.cbegin(), events.cend(), [&expectedId](const auto& event) {
        return event.terminal.has_value() && event.terminal->id == expectedId;
    });
    ASSERT_NE(events.cend(), started);
    ASSERT_NE(events.cend(), terminal);
    EXPECT_FALSE(std::find_if(started->activeActivities.cbegin(),
                              started->activeActivities.cend(),
                              [&expectedId](const auto& activity) { return activity.id == expectedId; })
                     ->canCancel);
    EXPECT_EQ(core::client::ActivityTerminalState::Completed, terminal->terminal->state);
    const core::client::CatalogPhotoPageResult photos = context.catalogEditorFacade.queryPhotoPage({});
    ASSERT_TRUE(photos.hasValue());
    EXPECT_EQ(1U, photos.value().photos.size());
}

TEST(QtActivityAdapterTest, UnsubscribeAndAdapterDestructionSuppressQueuedCallbacks)
{
    ActivityAdapterContext context;
    std::vector<core::client::ActivityEvent> events;
    const core::client::ActivitySubscriptionResult subscription = context.activityAdapter->subscribeToActivities(
        [&events](const core::client::ActivityEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    core::client::ActivitySubscriptionHandle handle = subscription.value();

    handle->unsubscribe();
    context.editorOrchestrator.previewStarted(9);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
    EXPECT_FALSE(handle->isActive());

    const core::client::ActivitySubscriptionResult outliving = context.activityAdapter->subscribeToActivities(
        [&events](const core::client::ActivityEvent& event) { events.push_back(event); });
    ASSERT_TRUE(outliving.hasValue());
    core::client::ActivitySubscriptionHandle outlivingHandle = outliving.value();
    context.activityAdapter.reset();
    EXPECT_FALSE(outlivingHandle->isActive());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
}

TEST(QtActivityAdapterTest, MainWindowConsumesActivitySnapshotInStatusBar)
{
    ActivityAdapterContext context;
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QSettings applicationSettings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")),
                                  QSettings::IniFormat);
    settings::QtWorkerProfileSettingsAdapter workerProfiles(applicationSettings);
    settings::QtExportSettingsAdapter exportDefaults(applicationSettings);
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    MainWindow window(
        context.catalogEditorFacade, exportAdapter, exportAdapter, exportDefaults, applicationSettings, workerProfiles);
    QLabel* label = window.findChild<QLabel*>(QStringLiteral("activityStatusLabel"));
    QProgressBar* progress = window.findChild<QProgressBar*>(QStringLiteral("activityProgressBar"));
    QToolButton* cancelButton = window.findChild<QToolButton*>(QStringLiteral("cancelActivityButton"));
    ASSERT_NE(nullptr, label);
    ASSERT_NE(nullptr, progress);
    ASSERT_NE(nullptr, cancelButton);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(label->isHidden());
    EXPECT_TRUE(progress->isHidden());
    EXPECT_TRUE(cancelButton->isHidden());

    context.editorOrchestrator.previewStarted(73);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_FALSE(label->isHidden());
    EXPECT_FALSE(progress->isHidden());
    EXPECT_FALSE(cancelButton->isHidden());
    EXPECT_EQ(QObject::tr("Rendering preview..."), label->text());

    context.editorOrchestrator.previewCompleted(73);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(label->isHidden());
    EXPECT_TRUE(progress->isHidden());
    EXPECT_TRUE(cancelButton->isHidden());
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

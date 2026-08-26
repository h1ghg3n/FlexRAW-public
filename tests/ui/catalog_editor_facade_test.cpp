#include <memory>
#include <utility>
#include <vector>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <gtest/gtest.h>

#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "editor_orchestrator.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"
#include "source_path.h"

namespace flexraw::ui::facade
{
namespace
{

class DormantPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: facade command test에서 실행될 필요가 없는 preview 요청을 구조화된 실패로 종료
    // 입력: request: preview 요청, tier: 요청 tier, cancellationToken: 취소 상태
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Facade test does not render previews.")});
    }
};

class ImmediatePreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: facade DisplayFrame projection test용 deterministic preview 생성
    // 입력: request/tier/cancellationToken: contract 일치를 위한 미사용 preview context
    // 출력: alpha normalization을 확인할 known-color 2x1 image
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        QImage image(2, 1, QImage::Format_RGBA8888);
        image.setPixelColor(0, 0, QColor(10, 20, 30, 40));
        image.setPixelColor(1, 0, QColor(50, 60, 70, 80));
        return core::orchestration::PreviewPipelineResult::success({std::move(image), {}, {}, {}});
    }
};

// 목적: catalog facade test에 사용할 source file 생성 또는 교체
// 입력: path: 기록할 file 경로, contents: fingerprint를 구분할 bytes
// 출력: 전체 bytes 기록 성공 여부
[[nodiscard]] bool writeSourceFile(const QString& path, const QByteArray& contents = QByteArray("facade-source"))
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size() &&
           file.flush();
}

// 목적: Qt event를 처리하며 facade source update 수가 목표에 도달할 때까지 대기
// 입력: updateCount: signal handler count, expectedCount: 목표 수, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 목표 update를 받으면 true
[[nodiscard]] bool waitForSourceUpdates(const int& updateCount, int expectedCount, int timeoutMs = 3000)
{
    QElapsedTimer timeout;
    timeout.start();
    while (timeout.elapsed() < timeoutMs)
    {
        if (updateCount >= expectedCount)
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return updateCount >= expectedCount;
}

// 목적: Qt delivery context를 처리하며 Editor event 수가 목표에 도달할 때까지 bounded wait
// 입력: events: callback이 누적한 event, expectedCount: 목표 수, timeoutMs: 제한 시간
// 출력: 제한 시간 안에 목표 event를 받으면 true
[[nodiscard]] bool waitForEditorEvents(const std::vector<core::client::EditorStateEvent>& events,
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

TEST(CatalogEditorFacadeTest, DeliversInitialEditorSnapshotAsynchronouslyOnQtContext)
{
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<DormantPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    std::vector<core::client::EditorStateEvent> events;
    bool subscribeReturned = false;
    bool reenteredSubscribe = false;
    QThread* callbackThread = nullptr;
    core::client::IEditorStateEventSource& eventSource = facade;

    const core::client::EditorStateSubscriptionResult subscription =
        eventSource.subscribeToEditorState([&](const core::client::EditorStateEvent& event) {
            reenteredSubscribe = !subscribeReturned;
            callbackThread = QThread::currentThread();
            events.push_back(event);
        });
    ASSERT_TRUE(subscription.hasValue());
    EXPECT_TRUE(events.empty());
    subscribeReturned = true;
    ASSERT_TRUE(waitForEditorEvents(events, 1));

    EXPECT_FALSE(reenteredSubscribe);
    EXPECT_EQ(facade.thread(), callbackThread);
    ASSERT_EQ(1U, events.size());
    EXPECT_TRUE(events.front().initial);
    EXPECT_GT(events.front().sequence.value, 0U);
    EXPECT_FALSE(events.front().snapshot.hasSelection);
    EXPECT_TRUE(subscription.value()->isActive());
}

TEST(CatalogEditorFacadeTest, CoalescesAdjustmentEventsAndPreservesFinalSnapshot)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("editor-events.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("editor-events.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<DormantPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    std::vector<core::client::EditorStateEvent> events;
    const core::client::EditorStateSubscriptionResult subscription = facade.subscribeToEditorState(
        [&events](const core::client::EditorStateEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());
    ASSERT_TRUE(waitForEditorEvents(events, 1));
    events.clear();
    const core::catalog::CatalogEntry entry{
        {sourcePath,
         QStringLiteral("jpg"),
         QStringLiteral("editor-events.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };
    int sourceUpdateCount = 0;
    QObject::connect(&catalogOrchestrator,
                     &core::orchestration::CatalogOrchestrator::sourceBindingUpdated,
                     &catalogOrchestrator,
                     [&sourceUpdateCount](const core::orchestration::CatalogSourceUpdate&) { ++sourceUpdateCount; });

    ASSERT_TRUE(facade.openCatalog(catalogPath).hasValue());
    const core::orchestration::CatalogImportResult imported = facade.importScannedEntries({entry});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    ASSERT_TRUE(waitForSourceUpdates(sourceUpdateCount, 1));
    core::client::IEditorClient& editorClient = facade;
    const core::client::EditorResult selected =
        editorClient.selectPhoto({core::client::ClientPhotoId{imported.value().photoIds.front().value}});
    ASSERT_TRUE(selected.hasValue());
    ASSERT_TRUE(waitForEditorEvents(events, 1));
    events.clear();

    ASSERT_TRUE(editorClient.beginAdjustment().hasValue());
    core::client::EditorDevelopParams params = selected.value().params;
    params.exposureEv = 0.2F;
    ASSERT_TRUE(editorClient.updateDevelopParams({params}).hasValue());
    params.exposureEv = 0.6F;
    ASSERT_TRUE(editorClient.updateDevelopParams({params}).hasValue());
    params.exposureEv = 1.0F;
    ASSERT_TRUE(editorClient.updateDevelopParams({params}).hasValue());
    ASSERT_TRUE(editorClient.endAdjustment().hasValue());
    EXPECT_TRUE(events.empty());
    ASSERT_TRUE(waitForEditorEvents(events, 2));

    ASSERT_EQ(2U, events.size());
    EXPECT_FALSE(events[0].initial);
    EXPECT_TRUE(events[0].snapshot.adjustmentActive);
    EXPECT_EQ(params, events[0].snapshot.params);
    EXPECT_FALSE(events[1].snapshot.adjustmentActive);
    EXPECT_EQ(params, events[1].snapshot.params);
    EXPECT_LT(events[0].sequence.value, events[1].sequence.value);
}

TEST(CatalogEditorFacadeTest, UnsubscribeSuppressesQueuedAndFutureEditorEvents)
{
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<DormantPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    std::vector<core::client::EditorStateEvent> events;
    const core::client::EditorStateSubscriptionResult subscription = facade.subscribeToEditorState(
        [&events](const core::client::EditorStateEvent& event) { events.push_back(event); });
    ASSERT_TRUE(subscription.hasValue());

    subscription.value()->unsubscribe();
    EXPECT_FALSE(subscription.value()->isActive());
    ASSERT_TRUE(facade.clearEditorSelection().hasValue());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    EXPECT_TRUE(events.empty());
}

TEST(CatalogEditorFacadeTest, AdapterDestructionDeactivatesOutlivingEditorSubscription)
{
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<DormantPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    std::vector<core::client::EditorStateEvent> events;
    core::client::EditorStateSubscriptionHandle handle;
    {
        auto facade = std::make_unique<CatalogEditorFacade>(
            catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
        const core::client::EditorStateSubscriptionResult subscription = facade->subscribeToEditorState(
            [&events](const core::client::EditorStateEvent& event) { events.push_back(event); });
        ASSERT_TRUE(subscription.hasValue());
        handle = subscription.value();
    }

    ASSERT_NE(nullptr, handle);
    EXPECT_FALSE(handle->isActive());
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_TRUE(events.empty());
}

TEST(CatalogEditorFacadeTest, ActivatesFolderPhotoWithStableIdentityAndForwardsEditorEvents)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("facade.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("facade.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    auto pipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    int stateChangeCount = 0;
    QObject::connect(&facade,
                     &CatalogEditorFacade::editorStateChanged,
                     &facade,
                     [&stateChangeCount](const core::orchestration::EditorState&) { ++stateChangeCount; });
    const core::catalog::CatalogEntry entry{
        {sourcePath, QStringLiteral("jpg"), QStringLiteral("facade.jpg"), core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(facade.openCatalog(catalogPath).hasValue());
    const core::orchestration::EditorStateResult selected = facade.activatePhoto(entry, QSize{640, 480});
    ASSERT_TRUE(selected.hasValue());
    core::types::DevelopParams params;
    params.exposureEv = 0.8F;
    ASSERT_TRUE(facade.updateDevelopParams(params, QSize{640, 480}));

    EXPECT_TRUE(selected.value().hasSelection);
    EXPECT_TRUE(core::types::isValidPhotoId(selected.value().photo.photoId));
    EXPECT_TRUE(selected.value().photo.transientKey.isEmpty());
    ASSERT_TRUE(facade.queryPhotos(core::catalog::CatalogPhotoPageRequest{}).hasValue());
    EXPECT_EQ(1, facade.queryPhotos(core::catalog::CatalogPhotoPageRequest{}).value().photos.size());
    EXPECT_EQ(params, facade.editorState().params);
    EXPECT_TRUE(facade.editorState().dirty);
    EXPECT_EQ(2, stateChangeCount);
}

TEST(CatalogEditorFacadeTest, ForwardsQtFreeEditorContractAndProjectsLegacyState)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("editor-client.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("editor-client.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<DormantPreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    const core::catalog::CatalogEntry entry{
        {sourcePath,
         QStringLiteral("jpg"),
         QStringLiteral("editor-client.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(facade.openCatalog(catalogPath).hasValue());
    const core::orchestration::CatalogImportResult imported = facade.importScannedEntries({entry});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    core::client::IEditorClient& editorClient = facade;
    const core::client::ClientPhotoId photoId{imported.value().photoIds.front().value};
    const core::client::EditorResult selected = editorClient.selectPhoto({photoId});
    ASSERT_TRUE(selected.hasValue());
    ASSERT_TRUE(editorClient.beginAdjustment().hasValue());
    core::client::EditorDevelopParams params = selected.value().params;
    params.exposureEv = 1.2F;
    ASSERT_TRUE(editorClient.updateDevelopParams({params}).hasValue());
    ASSERT_TRUE(editorClient.endAdjustment().hasValue());
    const core::client::EditorResult saved = editorClient.saveDevelopState();

    ASSERT_TRUE(saved.hasValue());
    EXPECT_EQ(photoId.value, facade.editorState().photo.photoId.value);
    EXPECT_EQ(params, facade.editorSnapshot().params);
    EXPECT_EQ(params.exposureEv, facade.editorState().params.exposureEv);
    EXPECT_FALSE(facade.editorState().dirty);
}

TEST(CatalogEditorFacadeTest, ProjectsAsyncPreviewIntoQtFreeDisplayFrame)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("frame.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("frame.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<ImmediatePreviewPipeline>());
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    std::optional<core::client::DisplayFrame> receivedFrame;
    QEventLoop eventLoop;
    QObject::connect(&facade,
                     &CatalogEditorFacade::displayFrameUpdated,
                     &eventLoop,
                     [&receivedFrame, &eventLoop](const core::client::DisplayFrame& frame) {
                         receivedFrame = frame;
                         eventLoop.quit();
                     });
    const core::catalog::CatalogEntry entry{
        {sourcePath, QStringLiteral("jpg"), QStringLiteral("frame.jpg"), core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(facade.openCatalog(catalogPath).hasValue());
    ASSERT_TRUE(facade.activatePhoto(entry, QSize{640, 480}).hasValue());
    QTimer::singleShot(3000, &eventLoop, &QEventLoop::quit);
    eventLoop.exec();

    ASSERT_TRUE(receivedFrame.has_value());
    EXPECT_EQ(2U, receivedFrame->width());
    EXPECT_EQ(1U, receivedFrame->height());
    EXPECT_GT(receivedFrame->previewRevision(), 0U);
    EXPECT_EQ(30U, receivedFrame->bytes()[0]);
    EXPECT_EQ(20U, receivedFrame->bytes()[1]);
    EXPECT_EQ(10U, receivedFrame->bytes()[2]);
    EXPECT_EQ(255U, receivedFrame->bytes()[3]);
}

TEST(CatalogEditorFacadeTest, ForwardsCatalogCommands)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("facade.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("facade.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath));
    auto pipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    const core::catalog::CatalogEntry entry{
        {sourcePath, QStringLiteral("jpg"), QStringLiteral("facade.jpg"), core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };

    ASSERT_TRUE(facade.openCatalog(catalogPath).hasValue());
    const core::orchestration::CatalogImportResult imported = facade.importScannedEntries({entry});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(1, imported.value().photoIds.size());
    const core::orchestration::CatalogPhotoPageResult photos =
        facade.queryPhotos(core::catalog::CatalogPhotoPageRequest{});

    ASSERT_TRUE(photos.hasValue());
    ASSERT_EQ(1, photos.value().photos.size());
    EXPECT_EQ(imported.value().photoIds.front().value, photos.value().photos.front().id.value);
    core::client::ICatalogProjectClient& projectClient = facade;
    const core::client::CatalogProjectResult project = projectClient.createProject({"Portfolio"});
    ASSERT_TRUE(project.hasValue());
    const core::client::ClientPhotoId photoId{imported.value().photoIds.front().value};
    ASSERT_TRUE(projectClient.addPhotoToProject({project.value().id, photoId}).hasValue());
    ASSERT_TRUE(projectClient.renameProject({project.value().id, "Selected"}).hasValue());
    const core::client::CatalogProjectListResult projects = projectClient.listProjects();
    ASSERT_TRUE(projects.hasValue());
    ASSERT_EQ(1, projects.value().size());
    EXPECT_EQ("Selected", projects.value().front().name);
    core::catalog::CatalogPhotoPageRequest projectRequest;
    projectRequest.projectId = core::catalog::ProjectId{project.value().id.value};
    ASSERT_TRUE(facade.queryPhotos(projectRequest).hasValue());
    EXPECT_EQ(1, facade.queryPhotos(projectRequest).value().photos.size());
    ASSERT_TRUE(projectClient.removePhotoFromProject({project.value().id, photoId}).hasValue());
    ASSERT_TRUE(projectClient.deleteProject({project.value().id}).hasValue());
    EXPECT_FALSE(facade.closeCatalog().isOpen);
    EXPECT_FALSE(facade.catalogState().isOpen);
}

TEST(CatalogEditorFacadeTest, ForwardsExactFolderPhotoPageScope)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString firstFolder = QDir(directory.path()).filePath(QStringLiteral("first"));
    const QString secondFolder = QDir(directory.path()).filePath(QStringLiteral("second"));
    ASSERT_TRUE(QDir().mkpath(firstFolder));
    ASSERT_TRUE(QDir().mkpath(secondFolder));
    const QString firstPath = QDir(firstFolder).filePath(QStringLiteral("first.jpg"));
    const QString secondPath = QDir(secondFolder).filePath(QStringLiteral("second.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("facade.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(firstPath));
    ASSERT_TRUE(writeSourceFile(secondPath));
    auto pipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    const core::catalog::CatalogEntry firstEntry{
        {firstPath, QStringLiteral("jpg"), QStringLiteral("first.jpg"), core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };
    const core::catalog::CatalogEntry secondEntry{
        {secondPath, QStringLiteral("jpg"), QStringLiteral("second.jpg"), core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };
    ASSERT_TRUE(facade.openCatalog(catalogPath).hasValue());
    ASSERT_TRUE(facade.importScannedEntries({firstEntry, secondEntry}).hasValue());
    core::catalog::CatalogPhotoPageRequest request;
    request.exactFolderPath = firstFolder;

    const core::orchestration::CatalogPhotoPageResult photos = facade.queryPhotos(request);
    const core::orchestration::CatalogFolderListResult folders = facade.queryFolders();

    ASSERT_TRUE(photos.hasValue());
    ASSERT_EQ(1, photos.value().photos.size());
    EXPECT_EQ(QStringLiteral("first.jpg"), photos.value().photos.front().displayName);
    ASSERT_TRUE(folders.hasValue());
    ASSERT_EQ(2, folders.value().size());
    EXPECT_EQ(core::catalog::normalizeSourceFolderPath(firstFolder), folders.value()[0].path);
    EXPECT_EQ(1, folders.value()[0].photoCount);
}

TEST(CatalogEditorFacadeTest, ForwardsSourceResolutionCommandsAndTerminalEvents)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath(QStringLiteral("replacement.jpg"));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("facade.flexraw-catalog"));
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("original-source")));
    auto pipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(pipeline));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, catalogThumbnailOrchestrator, editorOrchestrator);
    int updateCount = 0;
    core::orchestration::CatalogSourceUpdate latestUpdate;
    QObject::connect(&facade,
                     &CatalogEditorFacade::sourceBindingUpdated,
                     &facade,
                     [&updateCount, &latestUpdate](const core::orchestration::CatalogSourceUpdate& update) {
                         ++updateCount;
                         latestUpdate = update;
                     });
    const core::catalog::CatalogEntry entry{
        {sourcePath,
         QStringLiteral("jpg"),
         QStringLiteral("replacement.jpg"),
         core::types::SupportedFileKind::RasterImage},
        core::types::FileScanStatus::Ready,
    };
    ASSERT_TRUE(facade.openCatalog(catalogPath).hasValue());
    const core::orchestration::EditorStateResult activated = facade.activatePhoto(entry, QSize{640, 480});
    ASSERT_TRUE(activated.hasValue());
    ASSERT_TRUE(waitForSourceUpdates(updateCount, 1));
    const core::types::PhotoId photoId = activated.value().photo.photoId;
    ASSERT_TRUE(writeSourceFile(sourcePath, QByteArray("replacement-source-with-different-size")));
    ASSERT_TRUE(facade.selectCatalogPhoto(photoId, QSize{640, 480}).hasValue());
    ASSERT_TRUE(waitForSourceUpdates(updateCount, 2));
    ASSERT_EQ(core::catalog::SourceBindingState::ReplacementDetected, latestUpdate.photo.sourceState);

    const core::orchestration::CatalogSourceSubmissionResult accepted = facade.acceptReplacement(photoId);

    ASSERT_TRUE(accepted.hasValue());
    ASSERT_TRUE(waitForSourceUpdates(updateCount, 3));
    EXPECT_EQ(core::catalog::SourceBindingState::Available, latestUpdate.photo.sourceState);
    EXPECT_EQ(photoId.value, latestUpdate.photo.id.value);
}

}  // namespace
}  // namespace flexraw::ui::facade

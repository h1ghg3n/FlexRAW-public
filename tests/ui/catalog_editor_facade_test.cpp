#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "editor_orchestrator.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"

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
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, editorOrchestrator);
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
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, editorOrchestrator);
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
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, editorOrchestrator);
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

    ASSERT_TRUE(photos.hasValue());
    ASSERT_EQ(1, photos.value().photos.size());
    EXPECT_EQ(QStringLiteral("first.jpg"), photos.value().photos.front().displayName);
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
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    CatalogEditorFacade facade(catalogOrchestrator, editorOrchestrator);
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

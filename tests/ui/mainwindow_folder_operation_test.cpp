#include <functional>
#include <memory>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "catalog_editor_facade.h"
#include "catalog_list_widget.h"
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
#include "qt_export_client_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_source_resolution_event_source.h"
#include "qt_worker_profile_settings_adapter.h"
#include "test_path_identity_service.h"

namespace flexraw::ui::mainwindow
{
namespace
{

class DormantFolderPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: MainWindow Folder lifecycle test에서 실제 preview processing 생략
    // 입력: request/tier/cancellationToken: contract 일치를 위한 미사용 context
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Folder UI test does not render previews.")});
    }
};

// 목적: Qt event를 처리하며 presentation condition이 충족될 때까지 bounded wait
// 입력: condition: MainWindow 상태 predicate, timeoutMs: 제한 시간
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

class MainWindowFolderContext
{
public:
    // 목적: MainWindow Folder lifecycle test용 production-equivalent object graph 구성
    // 입력: 없음
    // 출력: MainWindow보다 dependencies가 오래 사는 deterministic context
    MainWindowFolderContext()
        : applicationSettings(QDir(settingsDirectory.path()).filePath(QStringLiteral("settings.ini")),
                              QSettings::IniFormat),
          exportDefaults(applicationSettings),
          workerProfiles(applicationSettings),
          previewOrchestrator(std::make_unique<DormantFolderPreviewPipeline>()),
          catalogThumbnailOrchestrator(std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>()),
          editorOrchestrator(previewOrchestrator, catalogOrchestrator),
          catalogSessionOrchestrator(catalogOrchestrator, editorOrchestrator),
          sourceResolutionEventSource(catalogOrchestrator),
          catalogEditorFacade(catalogOrchestrator,
                              catalogSessionOrchestrator,
                              catalogThumbnailOrchestrator,
                              editorOrchestrator,
                              sourceResolutionEventSource),
          exportOrchestrator(
              std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService())),
          exportAdapter(exportOrchestrator, workerProfiles),
          window(catalogEditorFacade, exportAdapter, exportAdapter, exportDefaults, applicationSettings, workerProfiles)
    {}

    QTemporaryDir settingsDirectory;
    QSettings applicationSettings;
    settings::QtExportSettingsAdapter exportDefaults;
    settings::QtWorkerProfileSettingsAdapter workerProfiles;
    core::orchestration::PreviewOrchestrator previewOrchestrator;
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator;
    core::orchestration::EditorOrchestrator editorOrchestrator;
    core::orchestration::CatalogSessionOrchestrator catalogSessionOrchestrator;
    core::orchestration::QtSourceResolutionEventSource sourceResolutionEventSource;
    facade::CatalogEditorFacade catalogEditorFacade;
    core::orchestration::ExportOrchestrator exportOrchestrator;
    export_::QtExportClientAdapter exportAdapter;
    MainWindow window;
};

TEST(MainWindowFolderOperationTest, ConsumesScanCompletionThroughQtFreeFolderContract)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QImage source(2, 2, QImage::Format_RGB32);
    source.fill(Qt::white);
    ASSERT_TRUE(source.save(QDir(directory.path()).filePath(QStringLiteral("folder-photo.jpg"))));
    MainWindowFolderContext context;
    auto* catalogList = context.window.findChild<catalog::CatalogListWidget*>();
    ASSERT_NE(nullptr, catalogList);

    const core::client::FolderOperationResult submitted =
        context.catalogEditorFacade.submitFolderScan({directory.path().toUtf8().toStdString()});

    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForCondition([catalogList] { return catalogList->count() == 1; }));
    ASSERT_EQ(1, catalogList->count());
    EXPECT_EQ(QStringLiteral("folder-photo.jpg"), catalogList->item(0)->text());
}

TEST(MainWindowFolderOperationTest, RefreshesCatalogOnlyAfterImportCompletion)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QImage source(2, 2, QImage::Format_RGB32);
    source.fill(Qt::white);
    ASSERT_TRUE(source.save(QDir(directory.path()).filePath(QStringLiteral("import-photo.jpg"))));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    MainWindowFolderContext context;
    ASSERT_TRUE(context.catalogOrchestrator.openCatalog(catalogPath).hasValue());
    auto* catalogList = context.window.findChild<catalog::CatalogListWidget*>();
    ASSERT_NE(nullptr, catalogList);
    const core::client::CatalogSessionSnapshot session = context.catalogEditorFacade.catalogSnapshot();

    const core::client::FolderOperationResult submitted =
        context.catalogEditorFacade.submitFolderImport({directory.path().toUtf8().toStdString(), session.catalogPath});

    ASSERT_TRUE(submitted.hasValue());
    ASSERT_TRUE(waitForCondition([catalogList] { return catalogList->count() == 1; }));
    const core::client::CatalogPhotoPageResult photos = context.catalogEditorFacade.queryPhotoPage({});
    ASSERT_TRUE(photos.hasValue());
    ASSERT_EQ(1U, photos.value().photos.size());
    EXPECT_EQ("import-photo.jpg", photos.value().photos.front().displayName);
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

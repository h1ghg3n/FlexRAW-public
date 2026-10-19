#include <memory>

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QMessageBox>
#include <QSettings>
#include <QTemporaryDir>

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
#include "preview_widget.h"
#include "qt_export_client_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_source_resolution_event_source.h"
#include "qt_worker_profile_settings_adapter.h"
#include "test_path_identity_service.h"

namespace flexraw::ui::mainwindow
{
namespace
{

class InactivePreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: Catalog session UI test에서 사용되지 않는 Preview processing 차단
    // 입력: request: 미사용 요청, tier: 미사용 tier, cancellationToken: 미사용 취소 상태
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Catalog session test does not render previews.")});
    }
};

TEST(MainWindowCatalogSessionTest, PresentsStartupRecoveryAndGuardsFolderBrowsingWithoutCatalog)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));

    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<InactivePreviewPipeline>());
    core::orchestration::CatalogThumbnailOrchestrator catalogThumbnailOrchestrator(
        std::make_unique<core::orchestration::FileCatalogThumbnailPipeline>());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    core::orchestration::CatalogSessionOrchestrator catalogSessionOrchestrator(catalogOrchestrator, editorOrchestrator);
    core::orchestration::QtSourceResolutionEventSource sourceResolutionEventSource(catalogOrchestrator);
    facade::CatalogEditorFacade catalogEditorFacade(catalogOrchestrator,
                                                    catalogSessionOrchestrator,
                                                    catalogThumbnailOrchestrator,
                                                    editorOrchestrator,
                                                    sourceResolutionEventSource);
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>(::flexraw::test::testPathIdentityService()));
    QSettings applicationSettings(QDir(directory.path()).filePath(QStringLiteral("settings.ini")),
                                  QSettings::IniFormat);
    settings::QtWorkerProfileSettingsAdapter workerProfiles(applicationSettings);
    settings::QtExportSettingsAdapter exportDefaults(applicationSettings);
    export_::QtExportClientAdapter exportAdapter(exportOrchestrator, workerProfiles);
    MainWindow window(catalogEditorFacade,
                      exportAdapter,
                      exportAdapter,
                      exportDefaults,
                      applicationSettings,
                      workerProfiles,
                      {CatalogStartupNoticeKind::RecoveredWithManagedCatalog,
                       QDir(directory.path()).filePath(QStringLiteral("missing.flexraw-catalog"))});

    QAction* closeAction = window.findChild<QAction*>(QStringLiteral("closeCatalogAction"));
    QAction* openFolderAction = window.findChild<QAction*>(QStringLiteral("openFolderAction"));
    QComboBox* folderScope = window.findChild<QComboBox*>(QStringLiteral("catalogFolderScopeComboBox"));
    QComboBox* projectScope = window.findChild<QComboBox*>(QStringLiteral("catalogProjectScopeComboBox"));
    auto* catalogList = window.findChild<catalog::CatalogListWidget*>();
    auto* preview = window.findChild<editor::PreviewWidget*>();

    ASSERT_NE(nullptr, closeAction);
    ASSERT_NE(nullptr, openFolderAction);
    ASSERT_NE(nullptr, folderScope);
    ASSERT_NE(nullptr, projectScope);
    ASSERT_NE(nullptr, catalogList);
    ASSERT_NE(nullptr, preview);
    EXPECT_TRUE(closeAction->isEnabled());
    EXPECT_TRUE(openFolderAction->isEnabled());
    EXPECT_TRUE(catalogSessionOrchestrator.catalogSnapshot().isOpen);

    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QMessageBox* startupNotice = window.findChild<QMessageBox*>(QStringLiteral("catalogStartupNotice"));
    ASSERT_NE(nullptr, startupNotice);
    EXPECT_EQ(QStringLiteral("Catalog Recovered"), startupNotice->windowTitle());
    EXPECT_TRUE(startupNotice->text().contains(QStringLiteral("managed catalog")));
    startupNotice->close();

    closeAction->trigger();

    EXPECT_FALSE(catalogSessionOrchestrator.catalogSnapshot().isOpen);
    EXPECT_FALSE(closeAction->isEnabled());
    EXPECT_FALSE(openFolderAction->isEnabled());
    EXPECT_FALSE(folderScope->isEnabled());
    EXPECT_FALSE(projectScope->isEnabled());
    EXPECT_EQ(0, folderScope->count());
    EXPECT_EQ(0, projectScope->count());
    EXPECT_EQ(0, catalogList->count());
    EXPECT_EQ(QStringLiteral("Open or create a catalog to browse photos."), preview->text());
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

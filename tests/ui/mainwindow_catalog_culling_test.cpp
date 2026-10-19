#include <memory>

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QItemSelectionModel>
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
#include "qt_export_client_adapter.h"
#include "qt_export_settings_adapter.h"
#include "qt_source_resolution_event_source.h"
#include "qt_worker_profile_settings_adapter.h"
#include "test_path_identity_service.h"

namespace flexraw::ui::mainwindow
{
namespace
{

class CullingPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: Catalog culling UI test에서 실제 Preview processing 생략
    // 입력: request: 미사용 요청, tier: 미사용 tier, cancellationToken: 미사용 취소 상태
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Culling test does not render previews.")});
    }
};

// 목적: Catalog culling UI test용 stable display order entry 생성
// 입력: folderPath: source parent path, index: identity와 표시 이름 순번
// 출력: Ready raster CatalogEntry
[[nodiscard]] core::catalog::CatalogEntry makeCullingEntry(const QString& folderPath, int index)
{
    const QString displayName = QStringLiteral("culling-%1.jpg").arg(index);
    return {
        core::types::FileDescriptor{
            QDir(folderPath).filePath(displayName),
            QStringLiteral("jpg"),
            displayName,
            core::types::SupportedFileKind::RasterImage,
        },
        core::types::FileScanStatus::Ready,
    };
}

TEST(MainWindowCatalogCullingTest, AppliesBatchStateAndFiltersPersistedResults)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("culling.flexraw-catalog"));
    const QString photoFolder = QDir(directory.path()).filePath(QStringLiteral("photos"));

    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    const core::orchestration::CatalogImportResult imported = catalogOrchestrator.importScannedEntries(
        {makeCullingEntry(photoFolder, 1), makeCullingEntry(photoFolder, 2), makeCullingEntry(photoFolder, 3)});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(3, imported.value().photoIds.size());

    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<CullingPreviewPipeline>());
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
    MainWindow window(
        catalogEditorFacade, exportAdapter, exportAdapter, exportDefaults, applicationSettings, workerProfiles);

    auto* catalogList = window.findChild<catalog::CatalogListWidget*>();
    QComboBox* filter = window.findChild<QComboBox*>(QStringLiteral("catalogCullingFilterComboBox"));
    QAction* pick = window.findChild<QAction*>(QStringLiteral("pickSelectedPhotosAction"));
    QAction* reject = window.findChild<QAction*>(QStringLiteral("rejectSelectedPhotosAction"));
    QAction* clear = window.findChild<QAction*>(QStringLiteral("clearSelectedCullingAction"));
    ASSERT_NE(nullptr, catalogList);
    ASSERT_NE(nullptr, filter);
    ASSERT_NE(nullptr, pick);
    ASSERT_NE(nullptr, reject);
    ASSERT_NE(nullptr, clear);
    ASSERT_EQ(3, catalogList->count());
    ASSERT_TRUE(filter->isEnabled());

    QItemSelectionModel* selection = catalogList->selectionModel();
    ASSERT_NE(nullptr, selection);
    selection->select(catalogList->model()->index(0, 0),
                      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->select(catalogList->model()->index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    ASSERT_TRUE(pick->isEnabled());
    pick->trigger();

    EXPECT_TRUE(catalogList->item(0)->text().startsWith(QStringLiteral("✓ ")));
    EXPECT_TRUE(catalogList->item(1)->text().startsWith(QStringLiteral("✓ ")));
    core::client::CatalogPhotoPageRequest pickedRequest;
    pickedRequest.cullingFilter = core::client::CatalogCullingFilter::Pick;
    const core::client::CatalogPhotoPageResult pickedPhotos = catalogEditorFacade.queryPhotoPage(pickedRequest);
    ASSERT_TRUE(pickedPhotos.hasValue());
    ASSERT_EQ(2U, pickedPhotos.value().photos.size());

    const int pickedIndex = filter->findData(static_cast<int>(core::client::CatalogCullingFilter::Pick));
    ASSERT_GE(pickedIndex, 0);
    filter->setCurrentIndex(pickedIndex);
    ASSERT_EQ(2, catalogList->count());
    catalogList->setCurrentRow(0);
    ASSERT_TRUE(reject->isEnabled());
    reject->trigger();
    EXPECT_EQ(1, catalogList->count());

    const int rejectedIndex = filter->findData(static_cast<int>(core::client::CatalogCullingFilter::Reject));
    ASSERT_GE(rejectedIndex, 0);
    filter->setCurrentIndex(rejectedIndex);
    ASSERT_EQ(1, catalogList->count());
    EXPECT_TRUE(catalogList->item(0)->text().startsWith(QStringLiteral("✕ ")));
    ASSERT_TRUE(clear->isEnabled());
    clear->trigger();
    EXPECT_EQ(0, catalogList->count());

    core::client::CatalogPhotoPageRequest rejectedRequest;
    rejectedRequest.cullingFilter = core::client::CatalogCullingFilter::Reject;
    const core::client::CatalogPhotoPageResult rejectedPhotos = catalogEditorFacade.queryPhotoPage(rejectedRequest);
    ASSERT_TRUE(rejectedPhotos.hasValue());
    EXPECT_TRUE(rejectedPhotos.value().photos.empty());
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

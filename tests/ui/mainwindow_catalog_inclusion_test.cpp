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

class InclusionPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: Catalog inclusion UI test에서 실제 Preview processing 생략
    // 입력: request: 미사용 요청, tier: 미사용 tier, cancellationToken: 미사용 취소 상태
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Inclusion test does not render previews.")});
    }
};

// 목적: Catalog inclusion UI test용 stable display order entry 생성
// 입력: folderPath: source parent path, index: identity와 표시 이름 순번
// 출력: Ready raster CatalogEntry
[[nodiscard]] core::catalog::CatalogEntry makeInclusionEntry(const QString& folderPath, int index)
{
    const QString displayName = QStringLiteral("inclusion-%1.jpg").arg(index);
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

TEST(MainWindowCatalogInclusionTest, ExcludesAndRestoresPhotosThroughSeparateViews)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("inclusion.flexraw-catalog"));
    const QString photoFolder = QDir(directory.path()).filePath(QStringLiteral("photos"));

    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    const core::orchestration::CatalogImportResult imported = catalogOrchestrator.importScannedEntries(
        {makeInclusionEntry(photoFolder, 1), makeInclusionEntry(photoFolder, 2), makeInclusionEntry(photoFolder, 3)});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(3, imported.value().photoIds.size());

    core::orchestration::PreviewOrchestrator previewOrchestrator(std::make_unique<InclusionPreviewPipeline>());
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
    QComboBox* filter = window.findChild<QComboBox*>(QStringLiteral("catalogInclusionFilterComboBox"));
    QAction* exclude = window.findChild<QAction*>(QStringLiteral("excludeSelectedPhotosAction"));
    QAction* restore = window.findChild<QAction*>(QStringLiteral("restoreSelectedPhotosAction"));
    ASSERT_NE(nullptr, catalogList);
    ASSERT_NE(nullptr, filter);
    ASSERT_NE(nullptr, exclude);
    ASSERT_NE(nullptr, restore);
    ASSERT_EQ(3, catalogList->count());
    ASSERT_TRUE(filter->isEnabled());
    EXPECT_FALSE(restore->isEnabled());

    QItemSelectionModel* selection = catalogList->selectionModel();
    ASSERT_NE(nullptr, selection);
    selection->select(catalogList->model()->index(0, 0),
                      QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    selection->select(catalogList->model()->index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    ASSERT_TRUE(exclude->isEnabled());
    exclude->trigger();

    ASSERT_EQ(1, catalogList->count());
    const core::client::EditorSnapshot includedEditor = catalogEditorFacade.editorSnapshot();
    ASSERT_TRUE(includedEditor.hasSelection);
    EXPECT_EQ(imported.value().photoIds.back().value, includedEditor.photoId.value);
    core::client::CatalogPhotoPageRequest excludedRequest;
    excludedRequest.inclusionFilter = core::client::CatalogInclusionFilter::ExcludedOnly;
    const core::client::CatalogPhotoPageResult excludedPhotos = catalogEditorFacade.queryPhotoPage(excludedRequest);
    ASSERT_TRUE(excludedPhotos.hasValue());
    ASSERT_EQ(2U, excludedPhotos.value().photos.size());

    const int excludedIndex =
        filter->findData(static_cast<int>(core::client::CatalogInclusionFilter::ExcludedOnly));
    ASSERT_GE(excludedIndex, 0);
    filter->setCurrentIndex(excludedIndex);
    ASSERT_EQ(2, catalogList->count());
    EXPECT_FALSE(exclude->isEnabled());
    ASSERT_TRUE(restore->isEnabled());
    restore->trigger();
    ASSERT_EQ(1, catalogList->count());

    const int includedIndex =
        filter->findData(static_cast<int>(core::client::CatalogInclusionFilter::IncludedOnly));
    ASSERT_GE(includedIndex, 0);
    filter->setCurrentIndex(includedIndex);
    ASSERT_EQ(2, catalogList->count());
    EXPECT_TRUE(exclude->isEnabled());
    EXPECT_FALSE(restore->isEnabled());

    const core::client::CatalogPhotoPageResult includedPhotos = catalogEditorFacade.queryPhotoPage({});
    ASSERT_TRUE(includedPhotos.hasValue());
    EXPECT_EQ(2U, includedPhotos.value().photos.size());
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

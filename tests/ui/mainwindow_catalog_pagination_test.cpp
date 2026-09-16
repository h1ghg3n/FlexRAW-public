#include <memory>

#include <QComboBox>
#include <QDir>
#include <QImage>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolButton>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_editor_facade.h"
#include "catalog_list_widget.h"
#include "catalog_orchestrator.h"
#include "catalog_photo_repository.h"
#include "catalog_session_orchestrator.h"
#include "catalog_thumbnail_orchestrator.h"
#include "catalog_thumbnail_pipeline.h"
#include "develop_panel.h"
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
#include "source_fingerprint.h"
#include "test_path_identity_service.h"

namespace flexraw::ui::mainwindow
{
namespace
{

class DormantPreviewPipeline final : public core::orchestration::IPreviewPipeline
{
public:
    // 목적: catalog pagination UI test에서 실제 preview processing 생략
    // 입력: request: 미사용 preview 요청, tier: 미사용 tier, cancellationToken: 미사용 취소 상태
    // 출력: test 전용 Cancelled 오류
    [[nodiscard]] core::orchestration::PreviewPipelineResult render(const core::orchestration::PreviewRequest&,
                                                                    core::orchestration::PreviewTier,
                                                                    const core::types::CancellationToken&) override
    {
        return core::orchestration::PreviewPipelineResult::failure(
            {core::types::ErrorCode::Cancelled, QStringLiteral("Pagination test does not render previews.")});
    }
};

// 목적: MainWindow page navigation test용 정렬 가능한 catalog entry 생성
// 입력: folderPath: source parent path, index: display name과 source path를 구분할 순번
// 출력: Ready 상태의 raster CatalogEntry
[[nodiscard]] core::catalog::CatalogEntry makeEntry(const QString& folderPath, int index)
{
    const QString displayName = QStringLiteral("photo-%1.jpg").arg(index, 3, 10, QLatin1Char('0'));
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

// 목적: MainWindow Folder scope test용 지정 folder의 정렬 가능한 catalog entry 생성
// 입력: folderPath: source parent path, index: display name을 구분할 순번
// 출력: Ready 상태의 raster CatalogEntry
[[nodiscard]] core::catalog::CatalogEntry makeFolderEntry(const QString& folderPath, int index)
{
    const QString displayName = QStringLiteral("photo-%1.jpg").arg(index, 3, 10, QLatin1Char('0'));
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

TEST(MainWindowCatalogPaginationTest, FinishesAndPersistsAdjustmentBeforePageNavigation)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString photoFolder = QDir(directory.path()).filePath(QStringLiteral("photos"));
    ASSERT_TRUE(QDir().mkpath(photoFolder));
    QImage source(1, 1, QImage::Format_RGB32);
    source.fill(Qt::white);
    ASSERT_TRUE(source.save(makeEntry(photoFolder, 0).file.path));
    ASSERT_TRUE(source.save(makeEntry(photoFolder, core::catalog::DefaultCatalogPhotoPageSize).file.path));
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository repository(*database.value());
        QVector<core::catalog::CatalogEntry> entries;
        entries.reserve(core::catalog::DefaultCatalogPhotoPageSize + 1);
        for (int index = 0; index <= core::catalog::DefaultCatalogPhotoPageSize; ++index)
        {
            entries.push_back(makeEntry(photoFolder, index));
        }
        ASSERT_TRUE(repository.upsert(entries).hasValue());
        core::types::CancellationSource cancellation;
        for (const int index : {0, core::catalog::DefaultCatalogPhotoPageSize})
        {
            const core::catalog::CatalogEntry entry = makeEntry(photoFolder, index);
            const core::catalog::CatalogPhotoRecordResult record = repository.findBySourcePath(entry.file.path);
            ASSERT_TRUE(record.hasValue());
            ASSERT_TRUE(record.value().has_value());
            const core::catalog::SourceFingerprintResult fingerprint = core::catalog::calculateSourceFingerprint(
                core::types::SourceLocator{entry.file.path}, cancellation.token());
            ASSERT_TRUE(fingerprint.hasValue());
            ASSERT_TRUE(repository.establishSourceFingerprint(record.value()->id, fingerprint.value()).hasValue());
        }
    }

    auto previewPipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(previewPipeline));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
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
    QToolButton* previousButton = window.findChild<QToolButton*>(QStringLiteral("catalogPreviousPageButton"));
    QToolButton* nextButton = window.findChild<QToolButton*>(QStringLiteral("catalogNextPageButton"));
    auto* developPanel = window.findChild<editor::DevelopPanel*>();
    QSlider* exposureSlider = window.findChild<QSlider*>(QStringLiteral("exposureSlider"));
    QSplitter* workspaceSplitter = window.findChild<QSplitter*>(QStringLiteral("workspaceSplitter"));
    int adjustmentFinishedCount = 0;

    ASSERT_NE(nullptr, previousButton);
    ASSERT_NE(nullptr, nextButton);
    ASSERT_NE(nullptr, developPanel);
    ASSERT_NE(nullptr, exposureSlider);
    ASSERT_NE(nullptr, workspaceSplitter);
    EXPECT_EQ(5, workspaceSplitter->handleWidth());
    EXPECT_TRUE(workspaceSplitter->styleSheet().contains(QStringLiteral("palette(mid)")));
    QObject::connect(developPanel, &editor::DevelopPanel::adjustmentFinished, [&adjustmentFinishedCount] {
        ++adjustmentFinishedCount;
    });
    EXPECT_FALSE(previousButton->isEnabled());
    EXPECT_TRUE(nextButton->isEnabled());

    ASSERT_TRUE(QMetaObject::invokeMethod(exposureSlider, "sliderPressed", Qt::DirectConnection));
    exposureSlider->setValue(10);
    nextButton->click();

    EXPECT_EQ(1, adjustmentFinishedCount);
    EXPECT_EQ(0, exposureSlider->value());
    EXPECT_TRUE(previousButton->isEnabled());
    EXPECT_FALSE(nextButton->isEnabled());

    ASSERT_TRUE(QMetaObject::invokeMethod(exposureSlider, "sliderPressed", Qt::DirectConnection));
    previousButton->click();

    EXPECT_EQ(2, adjustmentFinishedCount);
    EXPECT_EQ(10, exposureSlider->value());
    EXPECT_FALSE(previousButton->isEnabled());
    EXPECT_TRUE(nextButton->isEnabled());
}

TEST(MainWindowCatalogPaginationTest, SelectsExactFolderScopeAndKeepsScopedPageNavigation)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString firstFolder = QStringLiteral("C:/photos/first");
    const QString secondFolder = QStringLiteral("C:/photos/second");
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository repository(*database.value());
        QVector<core::catalog::CatalogEntry> entries;
        entries.reserve(core::catalog::DefaultCatalogPhotoPageSize + 2);
        for (int index = 0; index <= core::catalog::DefaultCatalogPhotoPageSize; ++index)
        {
            entries.push_back(makeFolderEntry(firstFolder, index));
        }
        entries.push_back(makeFolderEntry(secondFolder, 999));
        ASSERT_TRUE(repository.upsert(entries).hasValue());
    }

    auto previewPipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(previewPipeline));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
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
    QComboBox* scopeCombo = window.findChild<QComboBox*>(QStringLiteral("catalogFolderScopeComboBox"));
    auto* catalogList = window.findChild<catalog::CatalogListWidget*>();
    QToolButton* previousButton = window.findChild<QToolButton*>(QStringLiteral("catalogPreviousPageButton"));
    QToolButton* nextButton = window.findChild<QToolButton*>(QStringLiteral("catalogNextPageButton"));

    ASSERT_NE(nullptr, scopeCombo);
    ASSERT_NE(nullptr, catalogList);
    ASSERT_NE(nullptr, previousButton);
    ASSERT_NE(nullptr, nextButton);
    ASSERT_EQ(3, scopeCombo->count());
    EXPECT_EQ(core::catalog::DefaultCatalogPhotoPageSize, catalogList->count());
    EXPECT_TRUE(nextButton->isEnabled());

    const int firstFolderIndex = scopeCombo->findData(firstFolder);
    ASSERT_GT(firstFolderIndex, 0);
    scopeCombo->setCurrentIndex(firstFolderIndex);
    EXPECT_EQ(core::catalog::DefaultCatalogPhotoPageSize, catalogList->count());
    EXPECT_FALSE(previousButton->isEnabled());
    EXPECT_TRUE(nextButton->isEnabled());

    nextButton->click();
    EXPECT_EQ(1, catalogList->count());
    EXPECT_TRUE(previousButton->isEnabled());
    EXPECT_FALSE(nextButton->isEnabled());

    const int secondFolderIndex = scopeCombo->findData(secondFolder);
    ASSERT_GT(secondFolderIndex, 0);
    scopeCombo->setCurrentIndex(secondFolderIndex);
    EXPECT_EQ(1, catalogList->count());
    EXPECT_FALSE(previousButton->isEnabled());
    EXPECT_FALSE(nextButton->isEnabled());

    scopeCombo->setCurrentIndex(0);
    EXPECT_EQ(core::catalog::DefaultCatalogPhotoPageSize, catalogList->count());
    EXPECT_FALSE(previousButton->isEnabled());
    EXPECT_TRUE(nextButton->isEnabled());
}

TEST(MainWindowCatalogPaginationTest, SelectsProjectScopeAndDisablesFolderScope)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString firstFolder = QStringLiteral("C:/photos/first");
    const QString secondFolder = QStringLiteral("C:/photos/second");
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    const core::orchestration::CatalogImportResult imported = catalogOrchestrator.importScannedEntries(
        {makeFolderEntry(firstFolder, 1), makeFolderEntry(firstFolder, 2), makeFolderEntry(secondFolder, 3)});
    ASSERT_TRUE(imported.hasValue());
    ASSERT_EQ(3, imported.value().photoIds.size());
    const core::orchestration::CatalogProjectResult project =
        catalogOrchestrator.createProject(QStringLiteral("Portfolio"));
    ASSERT_TRUE(project.hasValue());
    ASSERT_TRUE(catalogOrchestrator.addPhotoToProject(project.value().id, imported.value().photoIds[0]).hasValue());
    ASSERT_TRUE(catalogOrchestrator.addPhotoToProject(project.value().id, imported.value().photoIds[2]).hasValue());

    auto previewPipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(previewPipeline));
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
    QComboBox* folderCombo = window.findChild<QComboBox*>(QStringLiteral("catalogFolderScopeComboBox"));
    QComboBox* projectCombo = window.findChild<QComboBox*>(QStringLiteral("catalogProjectScopeComboBox"));
    auto* catalogList = window.findChild<catalog::CatalogListWidget*>();

    ASSERT_NE(nullptr, folderCombo);
    ASSERT_NE(nullptr, projectCombo);
    ASSERT_NE(nullptr, catalogList);
    ASSERT_EQ(2, projectCombo->count());
    EXPECT_EQ(3, catalogList->count());
    EXPECT_TRUE(folderCombo->isEnabled());

    const int projectIndex = projectCombo->findData(project.value().id.value);
    ASSERT_GT(projectIndex, 0);
    projectCombo->setCurrentIndex(projectIndex);
    EXPECT_EQ(2, catalogList->count());
    EXPECT_FALSE(folderCombo->isEnabled());

    projectCombo->setCurrentIndex(0);
    EXPECT_EQ(3, catalogList->count());
    EXPECT_TRUE(folderCombo->isEnabled());
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

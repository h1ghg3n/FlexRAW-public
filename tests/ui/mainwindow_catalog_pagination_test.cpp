#include <memory>

#include <QDir>
#include <QTemporaryDir>
#include <QToolButton>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_editor_facade.h"
#include "catalog_orchestrator.h"
#include "catalog_photo_repository.h"
#include "editor_orchestrator.h"
#include "export_orchestrator.h"
#include "export_pipeline.h"
#include "mainwindow.h"
#include "preview_orchestrator.h"
#include "preview_pipeline.h"

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
// 입력: index: display name과 source path를 구분할 순번
// 출력: Ready 상태의 raster CatalogEntry
[[nodiscard]] core::catalog::CatalogEntry makeEntry(int index)
{
    const QString displayName = QStringLiteral("photo-%1.jpg").arg(index, 3, 10, QLatin1Char('0'));
    return {
        core::types::FileDescriptor{
            QStringLiteral("C:/photos/") + displayName,
            QStringLiteral("jpg"),
            displayName,
            core::types::SupportedFileKind::RasterImage,
        },
        core::types::FileScanStatus::Ready,
    };
}

TEST(MainWindowCatalogPaginationTest, EnablesOnlyAvailablePageDirections)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    {
        core::catalog::CatalogDatabaseOpenResult database = core::catalog::CatalogDatabase::open(catalogPath);
        ASSERT_TRUE(database.hasValue());
        core::catalog::CatalogPhotoRepository repository(*database.value());
        QVector<core::catalog::CatalogEntry> entries;
        entries.reserve(core::catalog::DefaultCatalogPhotoPageSize + 1);
        for (int index = 0; index <= core::catalog::DefaultCatalogPhotoPageSize; ++index)
        {
            entries.push_back(makeEntry(index));
        }
        ASSERT_TRUE(repository.upsert(entries).hasValue());
    }

    auto previewPipeline = std::make_unique<DormantPreviewPipeline>();
    core::orchestration::PreviewOrchestrator previewOrchestrator(std::move(previewPipeline));
    core::orchestration::CatalogOrchestrator catalogOrchestrator;
    ASSERT_TRUE(catalogOrchestrator.openCatalog(catalogPath).hasValue());
    core::orchestration::EditorOrchestrator editorOrchestrator(previewOrchestrator, catalogOrchestrator);
    facade::CatalogEditorFacade catalogEditorFacade(catalogOrchestrator, editorOrchestrator);
    core::orchestration::ExportOrchestrator exportOrchestrator(
        std::make_unique<core::orchestration::FileExportPipeline>());
    MainWindow window(catalogEditorFacade, catalogOrchestrator, exportOrchestrator);
    QToolButton* previousButton = window.findChild<QToolButton*>(QStringLiteral("catalogPreviousPageButton"));
    QToolButton* nextButton = window.findChild<QToolButton*>(QStringLiteral("catalogNextPageButton"));

    ASSERT_NE(nullptr, previousButton);
    ASSERT_NE(nullptr, nextButton);
    EXPECT_FALSE(previousButton->isEnabled());
    EXPECT_TRUE(nextButton->isEnabled());

    nextButton->click();

    EXPECT_TRUE(previousButton->isEnabled());
    EXPECT_FALSE(nextButton->isEnabled());

    previousButton->click();

    EXPECT_FALSE(previousButton->isEnabled());
    EXPECT_TRUE(nextButton->isEnabled());
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

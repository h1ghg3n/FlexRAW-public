#include <gtest/gtest.h>

#include "export_source_projection.h"

namespace flexraw::ui::mainwindow
{
namespace
{

// 목적: Export projection test용 current Editor state 생성
// 입력: sourcePath: current source path, photoId: current Catalog identity, exposureEv: unsaved in-memory exposure
// 출력: 처리 가능한 current Editor snapshot
[[nodiscard]] core::orchestration::EditorState makeEditorState(const QString& sourcePath,
                                                               core::types::PhotoId photoId,
                                                               float exposureEv)
{
    core::orchestration::EditorState state;
    state.hasSelection = true;
    state.photo.photoId = photoId;
    state.source = {
        sourcePath, QStringLiteral("jpg"), QStringLiteral("current.jpg"), core::types::SupportedFileKind::RasterImage};
    state.params.exposureEv = exposureEv;
    state.sourceProcessingAllowed = true;
    return state;
}

// 목적: Export projection test용 registered raster Photo 생성
// 입력: sourcePath: persisted source path, photoId: Catalog identity
// 출력: 처리 가능한 RasterImage Catalog record
[[nodiscard]] core::catalog::CatalogPhotoRecord makeCatalogRaster(const QString& sourcePath,
                                                                  core::types::PhotoId photoId)
{
    core::catalog::CatalogPhotoRecord photo;
    photo.id = photoId;
    photo.source = core::types::SourceLocator{sourcePath};
    photo.extension = QStringLiteral("jpg");
    photo.displayName = QStringLiteral("registered.jpg");
    photo.kind = core::types::SupportedFileKind::RasterImage;
    photo.scanStatus = core::types::FileScanStatus::Ready;
    photo.sourceState = core::catalog::SourceBindingState::Available;
    return photo;
}

TEST(ExportSourceProjectionTest, RegisteredNonCurrentRasterCarriesActiveCatalogWithoutExplicitOverride)
{
    const QString catalogPath = QStringLiteral("C:/catalog/library.flexraw-catalog");
    const core::orchestration::EditorState state =
        makeEditorState(QStringLiteral("C:/photos/current.jpg"), core::types::PhotoId{7}, 1.25F);
    const core::catalog::CatalogPhotoRecord photo =
        makeCatalogRaster(QStringLiteral("C:/photos/registered.jpg"), core::types::PhotoId{11});

    const std::optional<export_::ExportDialogSource> projected = makeCatalogExportSource(photo, state, catalogPath);

    ASSERT_TRUE(projected.has_value());
    EXPECT_EQ(catalogPath, projected->catalogPath);
    EXPECT_FALSE(projected->developParams.has_value());
    EXPECT_FALSE(projected->useDefaultDevelopParamsWhenCatalogPhotoMissing);
}

TEST(ExportSourceProjectionTest, UnregisteredNonCurrentRasterRequestsExplicitDefaultDevelopFallback)
{
    const QString catalogPath = QStringLiteral("C:/catalog/library.flexraw-catalog");
    const core::orchestration::EditorState state =
        makeEditorState(QStringLiteral("C:/photos/current.jpg"), core::types::PhotoId{7}, 1.25F);
    const core::catalog::CatalogEntry entry{{QStringLiteral("C:/photos/folder.png"),
                                             QStringLiteral("png"),
                                             QStringLiteral("folder.png"),
                                             core::types::SupportedFileKind::RasterImage},
                                            core::types::FileScanStatus::Ready};

    const std::optional<export_::ExportDialogSource> projected = makeFolderExportSource(entry, state, catalogPath);

    ASSERT_TRUE(projected.has_value());
    EXPECT_EQ(catalogPath, projected->catalogPath);
    EXPECT_FALSE(projected->developParams.has_value());
    EXPECT_TRUE(projected->useDefaultDevelopParamsWhenCatalogPhotoMissing);
}

TEST(ExportSourceProjectionTest, CurrentRasterKeepsInMemoryDevelopOverrideAboveCatalogState)
{
    const QString catalogPath = QStringLiteral("C:/catalog/library.flexraw-catalog");
    const core::orchestration::EditorState state =
        makeEditorState(QStringLiteral("C:/photos/current.jpg"), core::types::PhotoId{7}, 1.25F);
    const core::catalog::CatalogPhotoRecord photo =
        makeCatalogRaster(QStringLiteral("C:/photos/current.jpg"), core::types::PhotoId{7});

    const std::optional<export_::ExportDialogSource> projected = makeCatalogExportSource(photo, state, catalogPath);

    ASSERT_TRUE(projected.has_value());
    EXPECT_EQ(catalogPath, projected->catalogPath);
    ASSERT_TRUE(projected->developParams.has_value());
    EXPECT_FLOAT_EQ(1.25F, projected->developParams->exposureEv);
    EXPECT_FALSE(projected->useDefaultDevelopParamsWhenCatalogPhotoMissing);
}

TEST(ExportSourceProjectionTest, UnavailableCurrentDoesNotBlockIndependentRegisteredSelection)
{
    const QString catalogPath = QStringLiteral("C:/catalog/library.flexraw-catalog");
    core::orchestration::EditorState state;
    state.hasSelection = true;
    state.sourceProcessingAllowed = false;
    state.photo.photoId = core::types::PhotoId{7};
    const core::catalog::CatalogPhotoRecord photo =
        makeCatalogRaster(QStringLiteral("C:/photos/available.jpg"), core::types::PhotoId{11});

    const std::optional<export_::ExportDialogSource> projected = makeCatalogExportSource(photo, state, catalogPath);

    ASSERT_TRUE(projected.has_value());
    EXPECT_EQ(QStringLiteral("C:/photos/available.jpg"), projected->source.path);
    EXPECT_EQ(catalogPath, projected->catalogPath);
    EXPECT_FALSE(projected->developParams.has_value());
}

}  // namespace
}  // namespace flexraw::ui::mainwindow

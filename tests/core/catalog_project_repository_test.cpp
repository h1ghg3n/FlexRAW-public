#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_develop_repository.h"
#include "catalog_photo_repository.h"
#include "catalog_project_repository.h"

namespace flexraw::core::catalog
{
namespace
{

// 목적: Qt SQL test에 필요한 단일 QCoreApplication instance 보장
// 입력: 없음
// 출력: 없음
void ensureCoreApplication()
{
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }

    static int argumentCount = 1;
    static char applicationName[] = "flexraw_core_tests";
    static char* arguments[] = {applicationName, nullptr};
    static QCoreApplication application(argumentCount, arguments);
}

// 목적: Project repository test에서 저장할 CatalogEntry 생성
// 입력: path: source path, displayName: stable page sort에 사용할 표시 이름
// 출력: Ready raster CatalogEntry
[[nodiscard]] CatalogEntry makeEntry(const QString& path, const QString& displayName)
{
    return {
        types::FileDescriptor{
            path,
            QFileInfo(path).suffix().toLower(),
            displayName,
            types::SupportedFileKind::RasterImage,
        },
        types::FileScanStatus::Ready,
    };
}

// 목적: Develop state 보존 test에 사용할 완전한 source baseline 생성
// 입력: marker: SHA-256 byte 구분자
// 출력: 유효한 32 byte hash를 포함한 fingerprint
[[nodiscard]] types::SourceFingerprint makeFingerprint(char marker)
{
    return {10, 20, QByteArray(types::Sha256DigestSize, marker)};
}

class CatalogProjectRepositoryTest : public testing::Test
{
protected:
    // 목적: CatalogProjectRepository test suite 시작 전에 Qt core application 초기화
    // 입력: 없음
    // 출력: 없음
    static void SetUpTestSuite()
    {
        ensureCoreApplication();
    }
};

TEST_F(CatalogProjectRepositoryTest, CreatesRenamesAndListsProjectsInStableOrder)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    CatalogDatabaseOpenResult database =
        CatalogDatabase::open(QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog")));
    ASSERT_TRUE(database.hasValue());
    CatalogProjectRepository repository(*database.value());

    const CatalogProjectRecordResult beta = repository.createProject(QStringLiteral(" beta "));
    const CatalogProjectRecordResult alpha = repository.createProject(QStringLiteral("Alpha"));

    ASSERT_TRUE(beta.hasValue());
    ASSERT_TRUE(alpha.hasValue());
    EXPECT_TRUE(isValidProjectId(beta.value().id));
    EXPECT_EQ(QStringLiteral("beta"), beta.value().name);
    const CatalogProjectListResult projects = repository.queryProjects();
    ASSERT_TRUE(projects.hasValue());
    ASSERT_EQ(2, projects.value().size());
    EXPECT_EQ(alpha.value().id, projects.value()[0].id);
    EXPECT_EQ(beta.value().id, projects.value()[1].id);

    const CatalogProjectRecordResult renamed = repository.renameProject(beta.value().id, QStringLiteral("  Archive  "));
    ASSERT_TRUE(renamed.hasValue());
    EXPECT_EQ(QStringLiteral("Archive"), renamed.value().name);
    const CatalogProjectFindResult restored = repository.findById(beta.value().id);
    ASSERT_TRUE(restored.hasValue());
    ASSERT_TRUE(restored.value().has_value());
    EXPECT_EQ(QStringLiteral("Archive"), restored.value()->name);
}

TEST_F(CatalogProjectRepositoryTest, MaintainsManyToManyMembershipWithProjectScopedPaging)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    CatalogDatabaseOpenResult database =
        CatalogDatabase::open(QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog")));
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photos(*database.value());
    CatalogProjectRepository projects(*database.value());
    ASSERT_TRUE(photos
                    .upsert({
                        makeEntry(QStringLiteral("C:/photos/charlie.jpg"), QStringLiteral("charlie.jpg")),
                        makeEntry(QStringLiteral("C:/photos/alpha.jpg"), QStringLiteral("alpha.jpg")),
                        makeEntry(QStringLiteral("C:/photos/bravo.jpg"), QStringLiteral("bravo.jpg")),
                    })
                    .hasValue());
    const CatalogPhotoQueryResult allPhotos = photos.queryPage(CatalogPhotoPageRequest{});
    ASSERT_TRUE(allPhotos.hasValue());
    ASSERT_EQ(3, allPhotos.value().photos.size());
    const types::PhotoId alphaId = allPhotos.value().photos[0].id;
    const types::PhotoId bravoId = allPhotos.value().photos[1].id;
    const CatalogProjectRecordResult firstProject = projects.createProject(QStringLiteral("First"));
    const CatalogProjectRecordResult secondProject = projects.createProject(QStringLiteral("Second"));
    ASSERT_TRUE(firstProject.hasValue());
    ASSERT_TRUE(secondProject.hasValue());
    ASSERT_TRUE(projects.addPhoto(firstProject.value().id, alphaId).hasValue());
    ASSERT_TRUE(projects.addPhoto(firstProject.value().id, bravoId).hasValue());
    ASSERT_TRUE(projects.addPhoto(firstProject.value().id, alphaId).hasValue());
    ASSERT_TRUE(projects.addPhoto(secondProject.value().id, alphaId).hasValue());

    CatalogPhotoPageRequest request;
    request.pageSize = 1;
    request.projectId = firstProject.value().id;
    const CatalogPhotoQueryResult firstPage = photos.queryPage(request);
    ASSERT_TRUE(firstPage.hasValue());
    ASSERT_EQ(1, firstPage.value().photos.size());
    EXPECT_EQ(alphaId.value, firstPage.value().photos.front().id.value);
    ASSERT_TRUE(firstPage.value().nextCursor.has_value());
    EXPECT_EQ(firstProject.value().id, firstPage.value().nextCursor->projectId);

    request.cursor = firstPage.value().nextCursor;
    const CatalogPhotoQueryResult secondPage = photos.queryPage(request);
    ASSERT_TRUE(secondPage.hasValue());
    ASSERT_EQ(1, secondPage.value().photos.size());
    EXPECT_EQ(bravoId.value, secondPage.value().photos.front().id.value);
    EXPECT_FALSE(secondPage.value().nextCursor.has_value());

    request.projectId = secondProject.value().id;
    const CatalogPhotoQueryResult mismatchedScope = photos.queryPage(request);
    ASSERT_TRUE(mismatchedScope.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, mismatchedScope.error().code);

    ASSERT_TRUE(projects.removePhoto(firstProject.value().id, bravoId).hasValue());
    ASSERT_TRUE(projects.removePhoto(firstProject.value().id, bravoId).hasValue());
    request = CatalogPhotoPageRequest{};
    request.projectId = firstProject.value().id;
    const CatalogPhotoQueryResult remaining = photos.queryPage(request);
    ASSERT_TRUE(remaining.hasValue());
    ASSERT_EQ(1, remaining.value().photos.size());
    EXPECT_EQ(alphaId.value, remaining.value().photos.front().id.value);
}

TEST_F(CatalogProjectRepositoryTest, RemovingProjectPreservesPhotoAndDevelopState)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    CatalogDatabaseOpenResult database =
        CatalogDatabase::open(QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog")));
    ASSERT_TRUE(database.hasValue());
    CatalogPhotoRepository photos(*database.value());
    CatalogProjectRepository projects(*database.value());
    CatalogDevelopRepository develop(*database.value());
    const CatalogEntry entry = makeEntry(QStringLiteral("C:/photos/keep.jpg"), QStringLiteral("keep.jpg"));
    ASSERT_TRUE(photos.upsert({entry}).hasValue());
    const CatalogPhotoRecordResult stored = photos.findBySourcePath(entry.file.path);
    ASSERT_TRUE(stored.hasValue());
    ASSERT_TRUE(stored.value().has_value());
    const types::PhotoId photoId = stored.value()->id;
    ASSERT_TRUE(photos.establishSourceFingerprint(photoId, makeFingerprint('p')).hasValue());
    types::DevelopParams params;
    params.exposureEv = 0.75F;
    ASSERT_TRUE(develop.saveState(photoId, params, 0).hasValue());
    const CatalogProjectRecordResult project = projects.createProject(QStringLiteral("Temporary"));
    ASSERT_TRUE(project.hasValue());
    ASSERT_TRUE(projects.addPhoto(project.value().id, photoId).hasValue());

    const CatalogProjectMutationResult removed = projects.removeProject(project.value().id);

    ASSERT_TRUE(removed.hasValue());
    const CatalogProjectFindResult missingProject = projects.findById(project.value().id);
    ASSERT_TRUE(missingProject.hasValue());
    EXPECT_FALSE(missingProject.value().has_value());
    const CatalogPhotoRecordResult retainedPhoto = photos.findById(photoId);
    ASSERT_TRUE(retainedPhoto.hasValue());
    EXPECT_TRUE(retainedPhoto.value().has_value());
    const CatalogDevelopStateResult retainedDevelop = develop.loadState(photoId);
    ASSERT_TRUE(retainedDevelop.hasValue());
    ASSERT_TRUE(retainedDevelop.value().has_value());
    EXPECT_EQ(params, retainedDevelop.value()->params);
}

TEST_F(CatalogProjectRepositoryTest, RejectsInvalidNamesAndMissingMembershipTargets)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    CatalogDatabaseOpenResult database =
        CatalogDatabase::open(QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog")));
    ASSERT_TRUE(database.hasValue());
    CatalogProjectRepository projects(*database.value());

    const CatalogProjectRecordResult invalidName = projects.createProject(QStringLiteral("  "));
    const CatalogProjectMutationResult missingProject = projects.addPhoto(ProjectId{99}, types::PhotoId{88});

    ASSERT_TRUE(invalidName.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, invalidName.error().code);
    ASSERT_TRUE(missingProject.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, missingProject.error().code);
}

}  // namespace
}  // namespace flexraw::core::catalog

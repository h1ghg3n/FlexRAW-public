#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_folder_importer.h"
#include "catalog_photo_repository.h"

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

// 목적: folder import test에 사용할 빈 사진 파일 생성
// 입력: path: 생성할 지원 파일 경로
// 출력: 파일 생성 성공 여부
[[nodiscard]] bool createEmptyFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly);
}

class CatalogFolderImporterTest : public testing::Test
{
protected:
    // 목적: CatalogFolderImporter test suite 시작 전에 Qt core application 초기화
    // 입력: 없음
    // 출력: 없음
    static void SetUpTestSuite()
    {
        ensureCoreApplication();
    }
};

TEST_F(CatalogFolderImporterTest, ScansAndStoresSupportedPhotos)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(createEmptyFile(QDir(directory.path()).filePath(QStringLiteral("b.CR3"))));
    ASSERT_TRUE(createEmptyFile(QDir(directory.path()).filePath(QStringLiteral("a.jpg"))));
    ASSERT_TRUE(createEmptyFile(QDir(directory.path()).filePath(QStringLiteral("ignored.txt"))));
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogFolderImporter importer(*database.value());
    CatalogPhotoRepository repository(*database.value());

    const CatalogFolderImportResult imported = importer.importFolder(directory.path());
    const CatalogPhotoQueryResult stored = repository.queryPage(CatalogPhotoPageRequest{});

    ASSERT_TRUE(imported.hasValue());
    EXPECT_EQ(2, imported.value().storedCount);
    ASSERT_EQ(2, imported.value().entries.size());
    EXPECT_EQ(QStringLiteral("a.jpg"), imported.value().entries[0].file.displayName);
    ASSERT_TRUE(stored.hasValue());
    ASSERT_EQ(2, stored.value().photos.size());
    EXPECT_EQ(QStringLiteral("a.jpg"), stored.value().photos[0].displayName);
    EXPECT_EQ(QStringLiteral("b.CR3"), stored.value().photos[1].displayName);
}

TEST_F(CatalogFolderImporterTest, PropagatesFolderScanFailure)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult database = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(database.hasValue());
    CatalogFolderImporter importer(*database.value());

    const CatalogFolderImportResult result =
        importer.importFolder(QDir(directory.path()).filePath(QStringLiteral("missing")));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

}  // namespace
}  // namespace flexraw::core::catalog

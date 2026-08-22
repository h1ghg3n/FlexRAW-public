#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>

#include "catalog_database.h"
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

// 목적: v2와 v3 migration 검증을 위한 schema version 1 SQLite catalog 생성
// 입력: catalogPath: 생성할 SQLite database file 경로
// 출력: v1 catalog 생성 성공 여부
[[nodiscard]] bool createVersionOneCatalog(const QString& catalogPath)
{
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);

    if (!database.open())
    {
        return false;
    }

    bool created = false;

    {
        QSqlQuery query(database);
        created = query.exec(QStringLiteral("CREATE TABLE schema_version (version INTEGER NOT NULL)")) &&
                  query.exec(QStringLiteral("INSERT INTO schema_version (version) VALUES (1)"));
    }

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return created;
}

// 목적: v3 migration 검증을 위한 schema version 2 SQLite catalog 생성
// 입력: catalogPath: 생성할 SQLite database file 경로
// 출력: v2 catalog 생성 성공 여부
[[nodiscard]] bool createVersionTwoCatalog(const QString& catalogPath)
{
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);

    if (!database.open())
    {
        return false;
    }

    bool created = false;

    {
        QSqlQuery query(database);
        created = query.exec(QStringLiteral("CREATE TABLE schema_version (version INTEGER NOT NULL)")) &&
                  query.exec(QStringLiteral("INSERT INTO schema_version (version) VALUES (2)")) &&
                  query.exec(QStringLiteral("CREATE TABLE photos ("
                                            "id INTEGER PRIMARY KEY, "
                                            "path TEXT NOT NULL UNIQUE, "
                                            "extension TEXT NOT NULL, "
                                            "display_name TEXT NOT NULL, "
                                            "kind INTEGER NOT NULL, "
                                            "scan_status INTEGER NOT NULL, "
                                            "file_mtime_ms INTEGER NOT NULL, "
                                            "imported_at_ms INTEGER NOT NULL)"));
    }

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return created;
}

// 목적: v4 migration 검증을 위한 schema version 3 SQLite catalog 생성
// 입력: catalogPath: 생성할 SQLite database file 경로
// 출력: v3 catalog 생성 성공 여부
[[nodiscard]] bool createVersionThreeCatalog(const QString& catalogPath)
{
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);

    if (!database.open())
    {
        return false;
    }

    bool created = false;

    {
        QSqlQuery query(database);
        created = query.exec(QStringLiteral("CREATE TABLE schema_version (version INTEGER NOT NULL)")) &&
                  query.exec(QStringLiteral("INSERT INTO schema_version (version) VALUES (3)"));
    }

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return created;
}

// 목적: v5 migration 검증에 필요한 완전한 schema version 3 catalog 생성
// 입력: catalogPath: 생성할 SQLite database file 경로
// 출력: v3 photos·develop table 생성 성공 여부
[[nodiscard]] bool completeVersionThreeCatalog(const QString& catalogPath)
{
    if (!createVersionThreeCatalog(catalogPath))
    {
        return false;
    }

    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);
    if (!database.open())
    {
        return false;
    }

    bool completed = false;
    {
        QSqlQuery query(database);
        completed = query.exec(QStringLiteral("CREATE TABLE photos ("
                                              "id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, "
                                              "extension TEXT NOT NULL, display_name TEXT NOT NULL, "
                                              "kind INTEGER NOT NULL, scan_status INTEGER NOT NULL, "
                                              "file_mtime_ms INTEGER NOT NULL, imported_at_ms INTEGER NOT NULL)")) &&
                    query.exec(QStringLiteral("CREATE TABLE develop_params ("
                                              "photo_id INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE, "
                                              "params_json TEXT NOT NULL, updated_at_ms INTEGER NOT NULL)")) &&
                    query.exec(QStringLiteral("CREATE TABLE develop_history ("
                                              "id INTEGER PRIMARY KEY, "
                                              "photo_id INTEGER NOT NULL REFERENCES photos(id) ON DELETE CASCADE, "
                                              "step_id INTEGER NOT NULL, params_json TEXT NOT NULL, "
                                              "created_at_ms INTEGER NOT NULL, UNIQUE(photo_id, step_id))"));
    }

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return completed;
}

// 목적: stable photo identity v5 migration 검증용 schema version 4 catalog 생성
// 입력: catalogPath: 생성할 SQLite database file 경로
// 출력: v4 catalog과 migration 보존 검증용 photo·develop row 생성 여부
[[nodiscard]] bool createVersionFourCatalog(const QString& catalogPath)
{
    if (!completeVersionThreeCatalog(catalogPath))
    {
        return false;
    }

    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);
    if (!database.open())
    {
        return false;
    }

    bool created = false;
    {
        QSqlQuery query(database);
        created = query.exec(QStringLiteral("UPDATE schema_version SET version = 4")) &&
                  query.exec(QStringLiteral("CREATE TABLE presets ("
                                            "id INTEGER PRIMARY KEY, name TEXT NOT NULL, category TEXT NOT NULL, "
                                            "params_json TEXT NOT NULL, created_at_ms INTEGER NOT NULL, "
                                            "updated_at_ms INTEGER NOT NULL, UNIQUE(category, name))")) &&
                  query.exec(QStringLiteral(
                      "INSERT INTO photos (id, path, extension, display_name, kind, scan_status, file_mtime_ms, "
                      "imported_at_ms) VALUES (42, 'C:/photos/legacy.CR3', 'cr3', 'legacy.CR3', 1, 1, 1234, 5678)")) &&
                  query.exec(QStringLiteral("INSERT INTO develop_params (photo_id, params_json, updated_at_ms) "
                                            "VALUES (42, '{}', 5678)"));
    }

    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return created;
}

class CatalogDatabaseTest : public testing::Test
{
protected:
    // 목적: CatalogDatabase test suite 시작 전에 Qt core application 초기화
    // 입력: 없음
    // 출력: 없음
    static void SetUpTestSuite()
    {
        ensureCoreApplication();
    }
};

TEST_F(CatalogDatabaseTest, CreatesInitialSchemaForNewCatalog)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasValue());
    EXPECT_TRUE(QFileInfo::exists(catalogPath));
    const CatalogSchemaVersionResult version = result.value()->schemaVersion();
    ASSERT_TRUE(version.hasValue());
    EXPECT_EQ(7, version.value());
}

TEST_F(CatalogDatabaseTest, ReopensExistingCatalogWithoutReapplyingSchema)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));

    CatalogDatabaseOpenResult firstOpen = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(firstOpen.hasValue());
    firstOpen.value().reset();

    const CatalogDatabaseOpenResult secondOpen = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(secondOpen.hasValue());
    const CatalogSchemaVersionResult version = secondOpen.value()->schemaVersion();
    ASSERT_TRUE(version.hasValue());
    EXPECT_EQ(7, version.value());
}

TEST_F(CatalogDatabaseTest, MigratesVersionOneCatalogToCurrentSchema)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionOneCatalog(catalogPath));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasValue());
    const CatalogSchemaVersionResult version = result.value()->schemaVersion();
    ASSERT_TRUE(version.hasValue());
    EXPECT_EQ(7, version.value());
}

TEST_F(CatalogDatabaseTest, MigratesVersionTwoCatalogToCurrentSchema)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionTwoCatalog(catalogPath));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasValue());
    const CatalogSchemaVersionResult version = result.value()->schemaVersion();
    ASSERT_TRUE(version.hasValue());
    EXPECT_EQ(7, version.value());
}

TEST_F(CatalogDatabaseTest, MigratesVersionThreeCatalogToCurrentSchema)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(completeVersionThreeCatalog(catalogPath));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasValue());
    const CatalogSchemaVersionResult version = result.value()->schemaVersion();
    ASSERT_TRUE(version.hasValue());
    EXPECT_EQ(7, version.value());
}

TEST_F(CatalogDatabaseTest, MigratesVersionFourPhotoIdentityWithoutChangingPhotoId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));

    CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasValue());
    const CatalogSchemaVersionResult version = result.value()->schemaVersion();
    ASSERT_TRUE(version.hasValue());
    EXPECT_EQ(7, version.value());
    CatalogPhotoRepository repository(*result.value());
    const CatalogPhotoRecordResult migrated = repository.findById(types::PhotoId{42});
    ASSERT_TRUE(migrated.hasValue());
    ASSERT_TRUE(migrated.value().has_value());
    EXPECT_EQ(SourceBindingState::FingerprintPending, migrated.value()->sourceState);
    const types::SourceFingerprint trustedBaseline{2048, 9999, QByteArray(32, 'a')};
    ASSERT_TRUE(repository.establishSourceFingerprint(types::PhotoId{42}, trustedBaseline).hasValue());
    result.value().reset();

    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);
    ASSERT_TRUE(database.open());
    {
        QSqlQuery query(database);
        ASSERT_TRUE(
            query.exec(QStringLiteral("SELECT id, source_path, last_known_path, source_size_bytes, "
                                      "source_mtime_ms, source_sha256, source_binding_state, source_parent_path "
                                      "FROM photos WHERE id = 42")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(42, query.value(0).toLongLong());
        EXPECT_EQ(QStringLiteral("C:/photos/legacy.CR3"), query.value(1).toString());
        EXPECT_EQ(QStringLiteral("C:/photos/legacy.CR3"), query.value(2).toString());
        EXPECT_EQ(2048, query.value(3).toLongLong());
        EXPECT_EQ(9999, query.value(4).toLongLong());
        EXPECT_EQ(trustedBaseline.sha256, query.value(5).toByteArray());
        EXPECT_EQ(1, query.value(6).toInt());
        EXPECT_EQ(QStringLiteral("C:/photos"), query.value(7).toString());

        ASSERT_TRUE(
            query.exec(QStringLiteral("EXPLAIN QUERY PLAN SELECT id FROM photos WHERE source_parent_path = 'C:/photos' "
                                      "ORDER BY display_name COLLATE NOCASE, id LIMIT 100")));
        ASSERT_TRUE(query.next());
        EXPECT_TRUE(query.value(3).toString().contains(QStringLiteral("idx_photos_source_parent_display_name_id")));

        ASSERT_TRUE(query.exec(QStringLiteral("SELECT photo_id, revision FROM develop_params WHERE photo_id = 42")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(42, query.value(0).toLongLong());
        EXPECT_EQ(1, query.value(1).toLongLong());

        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA foreign_key_check")));
        EXPECT_FALSE(query.next());
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA foreign_key_list(develop_params)")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(QStringLiteral("photos"), query.value(2).toString());
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
}

TEST_F(CatalogDatabaseTest, RejectsEmptyCatalogPath)
{
    const CatalogDatabaseOpenResult result = CatalogDatabase::open(QStringLiteral(" "));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
}

TEST_F(CatalogDatabaseTest, RejectsMissingParentDirectory)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("missing/library.flexraw-catalog"));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::NotFound, result.error().code);
}

}  // namespace
}  // namespace flexraw::core::catalog

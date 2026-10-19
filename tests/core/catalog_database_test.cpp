#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>

#include <gtest/gtest.h>

#include "catalog_database.h"
#include "catalog_photo_repository.h"

namespace flexraw::core::catalog
{
namespace
{

constexpr int FlexRawCatalogApplicationId = 0x464C5852;

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

// 목적: Catalog test fixture에 추가 schema 또는 metadata SQL을 순서대로 적용
// 입력: catalogPath: 수정할 SQLite file 경로, statements: 실행할 SQL 목록
// 출력: 모든 SQL 실행 성공 여부
[[nodiscard]] bool executeCatalogStatements(const QString& catalogPath, const QStringList& statements)
{
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(catalogPath);
    if (!database.open())
    {
        return false;
    }

    bool succeeded = true;
    {
        QSqlQuery query(database);
        for (const QString& statement : statements)
        {
            if (!query.exec(statement))
            {
                succeeded = false;
                break;
            }
        }
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return succeeded;
}

// 목적: test Catalog 또는 migration backup에서 단일 영속 Catalog UUID 조회
// 입력: catalogPath: 조회할 SQLite file 경로
// 출력: canonical UUID text 또는 조회 실패 시 빈 문자열
[[nodiscard]] QString readCatalogUuidFromFile(const QString& catalogPath)
{
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(catalogPath);
    QString catalogUuid;
    if (database.open())
    {
        QSqlQuery query(database);
        if (query.exec(QStringLiteral("SELECT catalog_uuid FROM flexraw_catalog_metadata WHERE id = 1")) &&
            query.next())
        {
            catalogUuid = query.value(0).toString();
            if (query.next())
            {
                catalogUuid.clear();
            }
        }
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
    return catalogUuid;
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
    EXPECT_EQ(11, version.value());
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v0-to-v11.bak")));
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
    EXPECT_EQ(11, version.value());
}

TEST_F(CatalogDatabaseTest, PersistsCanonicalCatalogIdentityAcrossReopen)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult firstOpen = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(firstOpen.hasValue());
    firstOpen.value().reset();
    const QString firstUuid = readCatalogUuidFromFile(catalogPath);
    ASSERT_EQ(36, firstUuid.size());
    EXPECT_EQ(firstUuid, firstUuid.toLower());

    CatalogDatabaseOpenResult secondOpen = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(secondOpen.hasValue());
    secondOpen.value().reset();

    EXPECT_EQ(firstUuid, readCatalogUuidFromFile(catalogPath));
}

TEST_F(CatalogDatabaseTest, SerializesCatalogStartupWithoutHoldingSessionLock)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    QLockFile competingLock(catalogPath + QStringLiteral(".migration.lock"));
    competingLock.setStaleLockTime(0);
    ASSERT_TRUE(competingLock.tryLock(0));

    const CatalogDatabaseOpenResult blocked = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(blocked.hasError());
    EXPECT_EQ(types::ErrorCode::Conflict, blocked.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath));
    competingLock.unlock();

    CatalogDatabaseOpenResult opened = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(opened.hasValue());
    QLockFile sessionIndependentLock(catalogPath + QStringLiteral(".migration.lock"));
    sessionIndependentLock.setStaleLockTime(0);
    EXPECT_TRUE(sessionIndependentLock.tryLock(0));
}

TEST_F(CatalogDatabaseTest, StampsNewCatalogWithFlexRawApplicationId)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(result.hasValue());
    result.value().reset();

    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(catalogPath);
    ASSERT_TRUE(database.open());
    {
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA application_id")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(FlexRawCatalogApplicationId, query.value(0).toInt());
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
}

TEST_F(CatalogDatabaseTest, RejectsExistingSqliteWithoutCatalogSchemaWithoutMutatingIt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("other.sqlite"));
    ASSERT_TRUE(executeCatalogStatements(
        catalogPath,
        {QStringLiteral("CREATE TABLE external_data (id INTEGER PRIMARY KEY, value TEXT NOT NULL)"),
         QStringLiteral("INSERT INTO external_data (id, value) VALUES (1, 'preserved')")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(catalogPath);
    ASSERT_TRUE(database.open());
    {
        QSqlQuery query(database);
        EXPECT_FALSE(database.tables().contains(QStringLiteral("schema_version")));
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT value FROM external_data WHERE id = 1")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(QStringLiteral("preserved"), query.value(0).toString());
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA application_id")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(0, query.value(0).toInt());
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
}

TEST_F(CatalogDatabaseTest, RejectsAmbiguousSchemaVersionRows)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionOneCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(catalogPath, {QStringLiteral("INSERT INTO schema_version VALUES (1)")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v1-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, RejectsVersionedCatalogMissingRequiredSourceTable)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionOneCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(catalogPath, {QStringLiteral("UPDATE schema_version SET version = 2")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v2-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, RejectsCatalogMissingRequiredUniqueKey)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionOneCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(
        catalogPath,
        {QStringLiteral("UPDATE schema_version SET version = 2"),
         QStringLiteral("CREATE TABLE photos ("
                        "id INTEGER PRIMARY KEY, path TEXT NOT NULL, extension TEXT NOT NULL, "
                        "display_name TEXT NOT NULL, kind INTEGER NOT NULL, scan_status INTEGER NOT NULL, "
                        "file_mtime_ms INTEGER NOT NULL, imported_at_ms INTEGER NOT NULL)")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v2-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, RejectsCatalogMissingRequiredForeignKey)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionTwoCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(
        catalogPath,
        {QStringLiteral("UPDATE schema_version SET version = 3"),
         QStringLiteral("CREATE TABLE develop_params (photo_id INTEGER PRIMARY KEY, params_json TEXT NOT NULL, "
                        "updated_at_ms INTEGER NOT NULL)"),
         QStringLiteral("CREATE TABLE develop_history (id INTEGER PRIMARY KEY, photo_id INTEGER NOT NULL, "
                        "step_id INTEGER NOT NULL, params_json TEXT NOT NULL, created_at_ms INTEGER NOT NULL, "
                        "UNIQUE(photo_id, step_id))")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v3-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, RejectsAdditionalUniqueConstraintOnManagedTable)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(
        catalogPath, {QStringLiteral("CREATE UNIQUE INDEX external_photo_name_unique ON photos(display_name)")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v4-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, RejectsAdditionalColumnOnManagedTable)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    ASSERT_TRUE(
        executeCatalogStatements(catalogPath, {QStringLiteral("ALTER TABLE photos ADD COLUMN external_note TEXT")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v4-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, RejectsForeignApplicationIdBeforeMigration)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(catalogPath, {QStringLiteral("PRAGMA application_id = 1234")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v4-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, RejectsTriggerOnManagedTableBeforeMigration)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(
        catalogPath,
        {QStringLiteral("CREATE TRIGGER external_photo_trigger BEFORE UPDATE ON photos BEGIN SELECT 1; END")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v4-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, PreservesIndependentExtensionTableAcrossMigration)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(
        catalogPath,
        {QStringLiteral("CREATE TABLE external_notes (id INTEGER PRIMARY KEY, note TEXT NOT NULL)"),
         QStringLiteral("INSERT INTO external_notes (id, note) VALUES (1, 'preserved')")}));

    CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasValue());
    result.value().reset();
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(catalogPath);
    ASSERT_TRUE(database.open());
    {
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT note FROM external_notes WHERE id = 1")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(QStringLiteral("preserved"), query.value(0).toString());
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
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
    EXPECT_EQ(11, version.value());
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
    EXPECT_EQ(11, version.value());
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
    EXPECT_EQ(11, version.value());
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
    EXPECT_EQ(11, version.value());
    CatalogPhotoRepository repository(*result.value());
    const CatalogPhotoRecordResult migrated = repository.findById(types::PhotoId{42});
    ASSERT_TRUE(migrated.hasValue());
    ASSERT_TRUE(migrated.value().has_value());
    EXPECT_EQ(SourceBindingState::FingerprintPending, migrated.value()->sourceState);
    EXPECT_EQ(CatalogCullingState::Neutral, migrated.value()->cullingState);
    EXPECT_EQ(CatalogInclusionState::Included, migrated.value()->inclusionState);
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

        ASSERT_TRUE(query.exec(QStringLiteral("EXPLAIN QUERY PLAN SELECT source_parent_path, COUNT(*) FROM photos "
                                              "WHERE source_parent_path IS NOT NULL GROUP BY source_parent_path "
                                              "ORDER BY source_parent_path ASC")));
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

TEST_F(CatalogDatabaseTest, RejectsMalformedLegacyCatalogIdentityBeforeBackup)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    ASSERT_TRUE(executeCatalogStatements(
        catalogPath,
        {QStringLiteral("CREATE TABLE flexraw_catalog_metadata (id INTEGER PRIMARY KEY, catalog_uuid TEXT, note TEXT)"),
         QStringLiteral(
             "INSERT INTO flexraw_catalog_metadata (id, catalog_uuid, note) VALUES (1, 'not-a-uuid', 'x')")}));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath + QStringLiteral(".v4-to-v11.bak")));
}

TEST_F(CatalogDatabaseTest, CreatesVerifiedVersionedBackupBeforeMigration)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString backupPath = catalogPath + QStringLiteral(".v4-to-v11.bak");
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));

    CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasValue());
    result.value().reset();
    ASSERT_TRUE(QFileInfo::exists(backupPath));

    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(backupPath);
    ASSERT_TRUE(database.open());
    {
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA quick_check")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(QStringLiteral("ok"), query.value(0).toString());

        ASSERT_TRUE(query.exec(QStringLiteral("SELECT version FROM schema_version")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(4, query.value(0).toInt());

        ASSERT_TRUE(query.exec(QStringLiteral("SELECT source_version, target_version, created_at_ms, "
                                              "application_version, original_file_name, catalog_uuid "
                                              "FROM flexraw_migration_backup_metadata WHERE id = 1")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(4, query.value(0).toInt());
        EXPECT_EQ(11, query.value(1).toInt());
        EXPECT_GT(query.value(2).toLongLong(), 0);
        EXPECT_FALSE(query.value(3).toString().isEmpty());
        EXPECT_EQ(QStringLiteral("library.flexraw-catalog"), query.value(4).toString());
        EXPECT_EQ(readCatalogUuidFromFile(catalogPath), query.value(5).toString());
        EXPECT_EQ(readCatalogUuidFromFile(catalogPath), readCatalogUuidFromFile(backupPath));

        ASSERT_TRUE(query.exec(QStringLiteral("SELECT path FROM photos WHERE id = 42")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(QStringLiteral("C:/photos/legacy.CR3"), query.value(0).toString());
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
}

TEST_F(CatalogDatabaseTest, RejectsMigrationWhenExistingBackupIsInvalid)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString backupPath = catalogPath + QStringLiteral(".v4-to-v11.bak");
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    QFile invalidBackup(backupPath);
    ASSERT_TRUE(invalidBackup.open(QIODevice::WriteOnly));
    ASSERT_GT(invalidBackup.write("invalid backup"), 0);
    invalidBackup.close();

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, result.error().code);

    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(catalogPath);
    ASSERT_TRUE(database.open());
    {
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT version FROM schema_version")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(4, query.value(0).toInt());
    }
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
}

TEST_F(CatalogDatabaseTest, ReusesVerifiedMigrationBackupWithoutOverwritingIt)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString backupPath = catalogPath + QStringLiteral(".v4-to-v11.bak");
    ASSERT_TRUE(createVersionFourCatalog(catalogPath));
    CatalogDatabaseOpenResult firstMigration = CatalogDatabase::open(catalogPath);
    ASSERT_TRUE(firstMigration.hasValue());
    firstMigration.value().reset();

    QFile backupBefore(backupPath);
    ASSERT_TRUE(backupBefore.open(QIODevice::ReadOnly));
    const QByteArray originalBackup = backupBefore.readAll();
    backupBefore.close();
    ASSERT_FALSE(originalBackup.isEmpty());
    ASSERT_TRUE(QFile::remove(catalogPath));
    ASSERT_TRUE(QFile::copy(backupPath, catalogPath));

    const CatalogDatabaseOpenResult repeatedMigration = CatalogDatabase::open(catalogPath);

    ASSERT_TRUE(repeatedMigration.hasValue());
    QFile backupAfter(backupPath);
    ASSERT_TRUE(backupAfter.open(QIODevice::ReadOnly));
    EXPECT_EQ(originalBackup, backupAfter.readAll());
}

TEST_F(CatalogDatabaseTest, RejectsSameNameBackupFromDifferentCatalogIdentity)
{
    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    ASSERT_TRUE(firstDirectory.isValid());
    ASSERT_TRUE(secondDirectory.isValid());
    const QString firstCatalogPath = QDir(firstDirectory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString secondCatalogPath = QDir(secondDirectory.path()).filePath(QStringLiteral("library.flexraw-catalog"));
    const QString firstBackupPath = firstCatalogPath + QStringLiteral(".v4-to-v11.bak");
    const QString secondBackupPath = secondCatalogPath + QStringLiteral(".v4-to-v11.bak");
    ASSERT_TRUE(createVersionFourCatalog(firstCatalogPath));
    ASSERT_TRUE(createVersionFourCatalog(secondCatalogPath));
    CatalogDatabaseOpenResult firstMigration = CatalogDatabase::open(firstCatalogPath);
    ASSERT_TRUE(firstMigration.hasValue());
    firstMigration.value().reset();
    ASSERT_TRUE(QFile::copy(firstBackupPath, secondBackupPath));

    const CatalogDatabaseOpenResult secondMigration = CatalogDatabase::open(secondCatalogPath);

    ASSERT_TRUE(secondMigration.hasError());
    EXPECT_EQ(types::ErrorCode::DatabaseError, secondMigration.error().code);
    EXPECT_NE(readCatalogUuidFromFile(firstBackupPath), readCatalogUuidFromFile(secondCatalogPath));
    const QString connectionName =
        QStringLiteral("catalog_database_test_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(secondCatalogPath);
    ASSERT_TRUE(database.open());
    {
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT version FROM schema_version")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(4, query.value(0).toInt());
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

TEST_F(CatalogDatabaseTest, RejectsOuterWhitespaceWithoutOpeningASiblingCatalog)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalogPath = QDir(directory.path()).filePath(QStringLiteral("library.flexraw-catalog"));

    const CatalogDatabaseOpenResult result = CatalogDatabase::open(catalogPath + QLatin1Char(' '));

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(types::ErrorCode::InvalidArgument, result.error().code);
    EXPECT_FALSE(QFileInfo::exists(catalogPath));
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

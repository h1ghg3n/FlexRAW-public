#include "catalog_database.h"

#include <array>
#include <utility>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>
#include <QVector>

#include "source_path.h"

int qInitResources_catalog_migrations();

namespace
{

// 목적: static library에 포함된 catalog migration resource를 등록
// 입력: 없음
// 출력: 없음
void initializeCatalogMigrationResources()
{
    (void)qInitResources_catalog_migrations();
}

}  // namespace

namespace flexraw::core::catalog
{
namespace
{

struct CatalogMigration
{
    int version;
    const char* resourcePath;
};

constexpr std::array<CatalogMigration, 8> CatalogMigrations{
    CatalogMigration{1, ":/sql/migrations/v1__init.sql"},
    CatalogMigration{2, ":/sql/migrations/v2__create_photos.sql"},
    CatalogMigration{3, ":/sql/migrations/v3__create_develop_state.sql"},
    CatalogMigration{4, ":/sql/migrations/v4__create_presets.sql"},
    CatalogMigration{5, ":/sql/migrations/v5__separate_photo_identity.sql"},
    CatalogMigration{6, ":/sql/migrations/v6__add_develop_revision.sql"},
    CatalogMigration{7, ":/sql/migrations/v7__add_source_parent_path.sql"},
    CatalogMigration{8, ":/sql/migrations/v8__create_projects.sql"},
};

constexpr int CurrentSchemaVersion = CatalogMigrations.back().version;
constexpr int SourceParentBackfillBatchSize = 256;

struct SourceParentBackfillRecord
{
    qint64 photoId;
    QString sourcePath;
};

// 목적: database 관련 실패 정보를 일관된 CoreError로 생성
// 입력: message: 호출자에게 전달할 database 오류 설명
// 출력: DatabaseError로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeDatabaseError(QString message)
{
    return {types::ErrorCode::DatabaseError, std::move(message)};
}

// 목적: catalog database file 경로의 생성 가능 여부를 확인
// 입력: catalogPath: 확인할 database file 경로
// 출력: 정규화된 절대 경로 또는 구조화된 오류
[[nodiscard]] types::Result<QString, types::CoreError> validateCatalogPath(const QString& catalogPath)
{
    if (catalogPath.isEmpty() || catalogPath != catalogPath.trimmed())
    {
        return types::Result<QString, types::CoreError>::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Catalog path is empty or contains outer whitespace.")});
    }

    const QFileInfo catalogInfo(catalogPath);

    if (catalogInfo.exists() && catalogInfo.isDir())
    {
        return types::Result<QString, types::CoreError>::failure(
            {types::ErrorCode::InvalidArgument, QStringLiteral("Catalog path is a directory.")});
    }

    const QDir parentDirectory = catalogInfo.dir();

    if (!parentDirectory.exists())
    {
        return types::Result<QString, types::CoreError>::failure(
            {types::ErrorCode::NotFound, QStringLiteral("Catalog parent directory does not exist.")});
    }

    if (!QFileInfo(parentDirectory.absolutePath()).isWritable())
    {
        return types::Result<QString, types::CoreError>::failure(
            {types::ErrorCode::PermissionDenied, QStringLiteral("Catalog parent directory is not writable.")});
    }

    return types::Result<QString, types::CoreError>::success(catalogInfo.absoluteFilePath());
}

// 목적: Qt SQLite connection의 schema_version table에서 현재 version을 조회
// 입력: database: 열린 SQLite connection
// 출력: schema version 또는 구조화된 database 오류
[[nodiscard]] CatalogSchemaVersionResult readSchemaVersion(const QSqlDatabase& database)
{
    if (!database.tables().contains(QStringLiteral("schema_version")))
    {
        return CatalogSchemaVersionResult::success(0);
    }

    QSqlQuery query(database);

    if (!query.exec(QStringLiteral("SELECT version FROM schema_version LIMIT 1")) || !query.next())
    {
        return CatalogSchemaVersionResult::failure(makeDatabaseError(
            QStringLiteral("Unable to read the catalog schema version: %1").arg(query.lastError().text())));
    }

    return CatalogSchemaVersionResult::success(query.value(0).toInt());
}

// 목적: SQL migration text를 statement 단위로 분리
// 입력: migrationText: 세미콜론으로 종료된 SQL migration text
// 출력: 실행 순서를 보존한 SQL statement 목록
[[nodiscard]] QStringList splitSqlStatements(const QString& migrationText)
{
    QStringList statements;
    QString currentStatement;
    bool inStringLiteral = false;

    for (qsizetype index = 0; index < migrationText.size(); ++index)
    {
        const QChar character = migrationText.at(index);

        if (character == QLatin1Char('\''))
        {
            if (inStringLiteral && index + 1 < migrationText.size() && migrationText.at(index + 1) == QLatin1Char('\''))
            {
                currentStatement.append(character);
                currentStatement.append(migrationText.at(++index));
                continue;
            }

            inStringLiteral = !inStringLiteral;
        }

        if (character == QLatin1Char(';') && !inStringLiteral)
        {
            const QString statement = currentStatement.trimmed();

            if (!statement.isEmpty())
            {
                statements.append(statement);
            }

            currentStatement.clear();
            continue;
        }

        currentStatement.append(character);
    }

    const QString finalStatement = currentStatement.trimmed();

    if (!finalStatement.isEmpty())
    {
        statements.append(finalStatement);
    }

    return statements;
}

// 목적: 하나의 catalog migration resource를 transaction으로 실행
// 입력: database: migration을 적용할 열린 SQLite connection, migration: 적용할 version과 resource 경로
// 출력: 성공 표식 또는 구조화된 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> applyMigration(QSqlDatabase& database,
                                                                   const CatalogMigration& migration)
{
    initializeCatalogMigrationResources();
    QFile migrationFile(QString::fromLatin1(migration.resourcePath));

    if (!migrationFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to open catalog migration v%1.").arg(migration.version)));
    }

    const QStringList statements = splitSqlStatements(QString::fromUtf8(migrationFile.readAll()));

    if (statements.isEmpty())
    {
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog migration v%1 is empty.").arg(migration.version)));
    }

    if (!database.transaction())
    {
        return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to begin the catalog migration transaction: %1").arg(database.lastError().text())));
    }

    QSqlQuery query(database);

    for (const QString& statement : statements)
    {
        if (!query.exec(statement))
        {
            database.rollback();
            return types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Unable to apply catalog migration v%1: %2")
                                      .arg(migration.version)
                                      .arg(query.lastError().text())));
        }
    }

    if (!database.commit())
    {
        database.rollback();
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to commit catalog migration v%1: %2")
                                  .arg(migration.version)
                                  .arg(database.lastError().text())));
    }

    return types::Result<bool, types::CoreError>::success(true);
}

// 목적: 새 SQLite connection에 필요한 migration을 version 순서대로 적용
// 입력: database: migration을 적용할 열린 SQLite connection
// 출력: 성공 표식 또는 구조화된 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> applyMigrations(QSqlDatabase& database)
{
    const CatalogSchemaVersionResult currentVersion = readSchemaVersion(database);

    if (currentVersion.hasError())
    {
        return types::Result<bool, types::CoreError>::failure(currentVersion.error());
    }

    if (currentVersion.value() > CurrentSchemaVersion)
    {
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("The catalog schema version is newer than this application supports.")));
    }

    for (const CatalogMigration& migration : CatalogMigrations)
    {
        if (migration.version <= currentVersion.value())
        {
            continue;
        }

        const types::Result<bool, types::CoreError> appliedMigration = applyMigration(database, migration);

        if (appliedMigration.hasError())
        {
            return appliedMigration;
        }
    }

    return types::Result<bool, types::CoreError>::success(true);
}

// 목적: legacy photo row의 exact-folder query key를 bounded batch로 보완
// 입력: database: v7 schema가 적용된 열린 SQLite connection
// 출력: 모든 linked source를 backfill한 성공 표식 또는 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> backfillSourceParentPaths(QSqlDatabase& database)
{
    while (true)
    {
        QVector<SourceParentBackfillRecord> records;
        {
            QSqlQuery selectQuery(database);
            selectQuery.prepare(QStringLiteral("SELECT id, source_path FROM photos WHERE source_path IS NOT NULL "
                                               "AND source_parent_path IS NULL ORDER BY id LIMIT :batchSize"));
            selectQuery.bindValue(QStringLiteral(":batchSize"), SourceParentBackfillBatchSize);
            if (!selectQuery.exec())
            {
                return types::Result<bool, types::CoreError>::failure(
                    makeDatabaseError(QStringLiteral("Unable to read catalog source parent backfill rows: %1")
                                          .arg(selectQuery.lastError().text())));
            }
            while (selectQuery.next())
            {
                records.push_back({selectQuery.value(0).toLongLong(), selectQuery.value(1).toString()});
            }
        }

        if (records.isEmpty())
        {
            return types::Result<bool, types::CoreError>::success(true);
        }
        if (!database.transaction())
        {
            return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
                QStringLiteral("Unable to begin catalog source parent backfill: %1").arg(database.lastError().text())));
        }

        QSqlQuery updateQuery(database);
        if (!updateQuery.prepare(QStringLiteral("UPDATE photos SET source_parent_path = :sourceParentPath "
                                                "WHERE id = :photoId AND source_parent_path IS NULL")))
        {
            database.rollback();
            return types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Unable to prepare catalog source parent backfill: %1")
                                      .arg(updateQuery.lastError().text())));
        }

        for (const SourceParentBackfillRecord& record : records)
        {
            const QString parentPath = sourceParentPath(record.sourcePath);
            if (parentPath.isEmpty())
            {
                database.rollback();
                return types::Result<bool, types::CoreError>::failure(
                    makeDatabaseError(QStringLiteral("Catalog source path has no queryable parent folder.")));
            }
            updateQuery.bindValue(QStringLiteral(":sourceParentPath"), parentPath);
            updateQuery.bindValue(QStringLiteral(":photoId"), record.photoId);
            if (!updateQuery.exec() || updateQuery.numRowsAffected() != 1)
            {
                database.rollback();
                return types::Result<bool, types::CoreError>::failure(
                    makeDatabaseError(QStringLiteral("Unable to backfill catalog source parent path: %1")
                                          .arg(updateQuery.lastError().text())));
            }
        }

        if (!database.commit())
        {
            database.rollback();
            return types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Unable to commit catalog source parent backfill: %1")
                                      .arg(database.lastError().text())));
        }
    }
}

// 목적: 실패한 Qt SQLite connection을 안전하게 해제
// 입력: database: 해제할 Qt SQL handle, connectionName: Qt connection 식별자
// 출력: 없음
void removeConnection(QSqlDatabase& database, const QString& connectionName)
{
    database.close();
    database = QSqlDatabase{};
    QSqlDatabase::removeDatabase(connectionName);
}

}  // namespace

CatalogDatabase::CatalogDatabase(QSqlDatabase database, QString connectionName, QString catalogPath)
    : m_database(std::move(database)),
      m_connectionName(std::move(connectionName)),
      m_catalogPath(std::move(catalogPath))
{}

CatalogDatabase::~CatalogDatabase()
{
    removeConnection(m_database, m_connectionName);
}

CatalogDatabaseOpenResult CatalogDatabase::open(const QString& catalogPath)
{
    const types::Result<QString, types::CoreError> validatedPath = validateCatalogPath(catalogPath);

    if (validatedPath.hasError())
    {
        return CatalogDatabaseOpenResult::failure(validatedPath.error());
    }

    const QString connectionName = QStringLiteral("catalog_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(validatedPath.value());

    if (!database.open())
    {
        const types::CoreError error = makeDatabaseError(
            QStringLiteral("Unable to open the catalog database: %1").arg(database.lastError().text()));
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }

    QSqlQuery foreignKeyQuery(database);

    if (!foreignKeyQuery.exec(QStringLiteral("PRAGMA foreign_keys = ON")))
    {
        const types::CoreError error = makeDatabaseError(
            QStringLiteral("Unable to enable catalog foreign keys: %1").arg(foreignKeyQuery.lastError().text()));
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }

    const types::Result<bool, types::CoreError> migrated = applyMigrations(database);

    if (migrated.hasError())
    {
        const types::CoreError error = migrated.error();
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }

    const types::Result<bool, types::CoreError> backfilled = backfillSourceParentPaths(database);
    if (backfilled.hasError())
    {
        const types::CoreError error = backfilled.error();
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }

    return CatalogDatabaseOpenResult::success(
        CatalogDatabasePtr(new CatalogDatabase(std::move(database), connectionName, validatedPath.value())));
}

CatalogSchemaVersionResult CatalogDatabase::schemaVersion() const
{
    return readSchemaVersion(m_database);
}

const QString& CatalogDatabase::catalogPath() const noexcept
{
    return m_catalogPath;
}

}  // namespace flexraw::core::catalog

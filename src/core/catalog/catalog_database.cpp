#include "catalog_database.h"

#include <array>
#include <utility>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>
#include <QVector>

#include "catalog_identity.h"
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

constexpr std::array<CatalogMigration, 11> CatalogMigrations{
    CatalogMigration{1, ":/sql/migrations/v1__init.sql"},
    CatalogMigration{2, ":/sql/migrations/v2__create_photos.sql"},
    CatalogMigration{3, ":/sql/migrations/v3__create_develop_state.sql"},
    CatalogMigration{4, ":/sql/migrations/v4__create_presets.sql"},
    CatalogMigration{5, ":/sql/migrations/v5__separate_photo_identity.sql"},
    CatalogMigration{6, ":/sql/migrations/v6__add_develop_revision.sql"},
    CatalogMigration{7, ":/sql/migrations/v7__add_source_parent_path.sql"},
    CatalogMigration{8, ":/sql/migrations/v8__create_projects.sql"},
    CatalogMigration{9, ":/sql/migrations/v9__add_photo_culling.sql"},
    CatalogMigration{10, ":/sql/migrations/v10__add_photo_inclusion.sql"},
    CatalogMigration{11, ":/sql/migrations/v11__add_catalog_identity.sql"},
};

constexpr int CurrentSchemaVersion = CatalogMigrations.back().version;
constexpr int SourceParentBackfillBatchSize = 256;
constexpr int CatalogApplicationId = 0x464C5852;  // ASCII "FLXR"
constexpr auto MigrationBackupMetadataTable = "flexraw_migration_backup_metadata";

struct CatalogColumnRequirement
{
    int minimumVersion;
    int maximumVersion;
    const char* tableName;
    const char* columnName;
};

constexpr CatalogColumnRequirement RequiredCatalogColumns[]{
    {1, 11, "schema_version", "version"},
    {2, 4, "photos", "id"},
    {2, 4, "photos", "path"},
    {2, 4, "photos", "extension"},
    {2, 4, "photos", "display_name"},
    {2, 4, "photos", "kind"},
    {2, 4, "photos", "scan_status"},
    {2, 4, "photos", "file_mtime_ms"},
    {2, 4, "photos", "imported_at_ms"},
    {5, 11, "photos", "id"},
    {5, 11, "photos", "source_path"},
    {5, 11, "photos", "last_known_path"},
    {5, 11, "photos", "extension"},
    {5, 11, "photos", "display_name"},
    {5, 11, "photos", "kind"},
    {5, 11, "photos", "scan_status"},
    {5, 11, "photos", "source_size_bytes"},
    {5, 11, "photos", "source_mtime_ms"},
    {5, 11, "photos", "source_sha256"},
    {5, 11, "photos", "source_binding_state"},
    {5, 11, "photos", "imported_at_ms"},
    {7, 11, "photos", "source_parent_path"},
    {9, 11, "photos", "culling_state"},
    {10, 11, "photos", "inclusion_state"},
    {3, 11, "develop_params", "photo_id"},
    {3, 11, "develop_params", "params_json"},
    {3, 11, "develop_params", "updated_at_ms"},
    {6, 11, "develop_params", "revision"},
    {3, 11, "develop_history", "id"},
    {3, 11, "develop_history", "photo_id"},
    {3, 11, "develop_history", "step_id"},
    {3, 11, "develop_history", "params_json"},
    {3, 11, "develop_history", "created_at_ms"},
    {4, 11, "presets", "id"},
    {4, 11, "presets", "name"},
    {4, 11, "presets", "category"},
    {4, 11, "presets", "params_json"},
    {4, 11, "presets", "created_at_ms"},
    {4, 11, "presets", "updated_at_ms"},
    {8, 11, "projects", "id"},
    {8, 11, "projects", "name"},
    {8, 11, "project_photos", "project_id"},
    {8, 11, "project_photos", "photo_id"},
    {11, 11, "flexraw_catalog_metadata", "id"},
    {11, 11, "flexraw_catalog_metadata", "catalog_uuid"},
};

struct CatalogKeyRequirement
{
    int minimumVersion;
    int maximumVersion;
    const char* tableName;
    const char* columns;
};

constexpr CatalogKeyRequirement RequiredCatalogPrimaryKeys[]{
    {2, 11, "photos", "id"},
    {3, 11, "develop_params", "photo_id"},
    {3, 11, "develop_history", "id"},
    {4, 11, "presets", "id"},
    {8, 11, "projects", "id"},
    {8, 11, "project_photos", "project_id,photo_id"},
    {11, 11, "flexraw_catalog_metadata", "id"},
};

constexpr CatalogKeyRequirement RequiredCatalogUniqueKeys[]{
    {2, 4, "photos", "path"},
    {5, 11, "photos", "source_path"},
    {3, 11, "develop_history", "photo_id,step_id"},
    {4, 11, "presets", "category,name"},
    {11, 11, "flexraw_catalog_metadata", "catalog_uuid"},
};

struct CatalogForeignKeyRequirement
{
    int minimumVersion;
    int maximumVersion;
    const char* tableName;
    const char* signature;
};

constexpr CatalogForeignKeyRequirement RequiredCatalogForeignKeys[]{
    {3, 11, "develop_params", "photo_id>photos.id|NO ACTION|CASCADE"},
    {3, 11, "develop_history", "photo_id>photos.id|NO ACTION|CASCADE"},
    {8, 11, "project_photos", "project_id>projects.id|NO ACTION|CASCADE"},
    {8, 11, "project_photos", "photo_id>photos.id|NO ACTION|CASCADE"},
};

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

// 목적: 열린 SQLite file의 application_id를 조회
// 입력: database: 확인할 열린 SQLite connection
// 출력: application_id 또는 구조화된 database 오류
[[nodiscard]] types::Result<int, types::CoreError> readCatalogApplicationId(const QSqlDatabase& database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA application_id")) || !query.next())
    {
        return types::Result<int, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to read the catalog application id: %1").arg(query.lastError().text())));
    }

    bool converted = false;
    const int applicationId = query.value(0).toInt(&converted);
    if (!converted || query.next())
    {
        return types::Result<int, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog application id is invalid.")));
    }
    return types::Result<int, types::CoreError>::success(applicationId);
}

// 목적: 검증된 Catalog에 FlexRAW application_id를 기록
// 입력: database: 기록할 열린 SQLite connection
// 출력: 기록 성공 표식 또는 구조화된 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> writeCatalogApplicationId(QSqlDatabase& database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA application_id = %1").arg(CatalogApplicationId)))
    {
        return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to write the catalog application id: %1").arg(query.lastError().text())));
    }

    const types::Result<int, types::CoreError> applicationId = readCatalogApplicationId(database);
    if (applicationId.hasError())
    {
        return types::Result<bool, types::CoreError>::failure(applicationId.error());
    }
    if (applicationId.value() != CatalogApplicationId)
    {
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog application id was not persisted.")));
    }
    return types::Result<bool, types::CoreError>::success(true);
}

// 목적: 지정 table의 실제 column 이름 집합을 조회
// 입력: database: 열린 SQLite connection, tableName: 확인할 project-owned table 이름
// 출력: column 이름 집합 또는 구조화된 database 오류
[[nodiscard]] types::Result<QSet<QString>, types::CoreError> readCatalogTableColumns(const QSqlDatabase& database,
                                                                                     const QString& tableName)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_xinfo(\"%1\")").arg(tableName)))
    {
        return types::Result<QSet<QString>, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to inspect catalog table %1: %2").arg(tableName, query.lastError().text())));
    }

    QSet<QString> columns;
    while (query.next())
    {
        columns.insert(query.value(1).toString());
    }
    if (columns.isEmpty())
    {
        return types::Result<QSet<QString>, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Required catalog table %1 is missing.").arg(tableName)));
    }
    return types::Result<QSet<QString>, types::CoreError>::success(std::move(columns));
}

// 목적: SQLite identifier를 PRAGMA statement에서 안전하게 사용할 수 있도록 인용
// 입력: identifier: database에서 읽은 table 또는 index 이름
// 출력: 큰따옴표 escaping을 적용한 identifier
[[nodiscard]] QString quoteSqliteIdentifier(QString identifier)
{
    identifier.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QStringLiteral("\"%1\"").arg(identifier);
}

// 목적: SQLite ordinal별 column 이름을 key signature로 직렬화
// 입력: columns: key 순서를 key로 보관한 column map
// 출력: 쉼표로 연결한 ordered column signature
[[nodiscard]] QString catalogKeySignature(const QMap<int, QString>& columns)
{
    QStringList orderedColumns;
    orderedColumns.reserve(columns.size());
    for (auto column = columns.cbegin(); column != columns.cend(); ++column)
    {
        orderedColumns.append(column.value());
    }
    return orderedColumns.join(QLatin1Char(','));
}

// 목적: managed table의 실제 primary key column 순서를 조회
// 입력: database: 열린 SQLite connection, tableName: 확인할 managed table
// 출력: primary key signature 또는 구조화된 database 오류
[[nodiscard]] types::Result<QString, types::CoreError> readCatalogPrimaryKey(const QSqlDatabase& database,
                                                                             const QString& tableName)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_xinfo(%1)").arg(quoteSqliteIdentifier(tableName))))
    {
        return types::Result<QString, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to inspect primary key for catalog table %1: %2")
                                  .arg(tableName, query.lastError().text())));
    }

    QMap<int, QString> primaryKeyColumns;
    while (query.next())
    {
        const int primaryKeyOrdinal = query.value(5).toInt();
        if (primaryKeyOrdinal > 0)
        {
            primaryKeyColumns.insert(primaryKeyOrdinal, query.value(1).toString());
        }
    }
    return types::Result<QString, types::CoreError>::success(catalogKeySignature(primaryKeyColumns));
}

// 목적: managed table의 실제 unique key와 SQLite origin을 조회
// 입력: database: 열린 SQLite connection, tableName: 확인할 managed table
// 출력: column signature별 index origin 또는 구조화된 database 오류
[[nodiscard]] types::Result<QHash<QString, QString>, types::CoreError> readCatalogUniqueKeys(
    const QSqlDatabase& database, const QString& tableName)
{
    QSqlQuery indexListQuery(database);
    if (!indexListQuery.exec(QStringLiteral("PRAGMA index_list(%1)").arg(quoteSqliteIdentifier(tableName))))
    {
        return types::Result<QHash<QString, QString>, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to inspect unique keys for catalog table %1: %2")
                                  .arg(tableName, indexListQuery.lastError().text())));
    }

    QHash<QString, QString> uniqueKeys;
    while (indexListQuery.next())
    {
        if (indexListQuery.value(2).toInt() == 0)
        {
            continue;
        }

        const QString indexName = indexListQuery.value(1).toString();
        QSqlQuery indexInfoQuery(database);
        if (!indexInfoQuery.exec(QStringLiteral("PRAGMA index_info(%1)").arg(quoteSqliteIdentifier(indexName))))
        {
            return types::Result<QHash<QString, QString>, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Unable to inspect catalog index %1: %2")
                                      .arg(indexName, indexInfoQuery.lastError().text())));
        }

        QMap<int, QString> indexColumns;
        while (indexInfoQuery.next())
        {
            indexColumns.insert(indexInfoQuery.value(0).toInt(), indexInfoQuery.value(2).toString());
        }
        const QString origin = indexListQuery.value(4).toInt() == 0
                                   ? indexListQuery.value(3).toString()
                                   : QStringLiteral("%1:partial").arg(indexListQuery.value(3).toString());
        uniqueKeys.insert(catalogKeySignature(indexColumns), origin);
    }
    return types::Result<QHash<QString, QString>, types::CoreError>::success(std::move(uniqueKeys));
}

// 목적: managed table의 foreign key 동작 signature를 조회
// 입력: database: 열린 SQLite connection, tableName: 확인할 managed table
// 출력: foreign key signature 집합 또는 구조화된 database 오류
[[nodiscard]] types::Result<QSet<QString>, types::CoreError> readCatalogForeignKeys(const QSqlDatabase& database,
                                                                                    const QString& tableName)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA foreign_key_list(%1)").arg(quoteSqliteIdentifier(tableName))))
    {
        return types::Result<QSet<QString>, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to inspect foreign keys for catalog table %1: %2")
                                  .arg(tableName, query.lastError().text())));
    }

    QSet<QString> foreignKeys;
    while (query.next())
    {
        foreignKeys.insert(QStringLiteral("%1>%2.%3|%4|%5")
                               .arg(query.value(3).toString(),
                                    query.value(2).toString(),
                                    query.value(4).toString(),
                                    query.value(5).toString().toUpper(),
                                    query.value(6).toString().toUpper()));
    }
    return types::Result<QSet<QString>, types::CoreError>::success(std::move(foreignKeys));
}

// 목적: source version에서 필요한 최소 Catalog 구조와 managed table trigger 부재를 검증
// 입력: database: 열린 SQLite connection, schemaVersion: 검증할 source schema version
// 출력: 필수 계약 충족 표식 또는 구조화된 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> validateRequiredCatalogSchema(const QSqlDatabase& database,
                                                                                  int schemaVersion)
{
    if (schemaVersion <= 0 || schemaVersion > CurrentSchemaVersion)
    {
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog schema version is outside the supported range.")));
    }

    QHash<QString, QSet<QString>> tableColumns;
    QHash<QString, QSet<QString>> expectedColumns;
    for (const CatalogColumnRequirement& requirement : RequiredCatalogColumns)
    {
        if (schemaVersion < requirement.minimumVersion || schemaVersion > requirement.maximumVersion)
        {
            continue;
        }

        const QString tableName = QString::fromLatin1(requirement.tableName);
        if (!tableColumns.contains(tableName))
        {
            const types::Result<QSet<QString>, types::CoreError> columns = readCatalogTableColumns(database, tableName);
            if (columns.hasError())
            {
                return types::Result<bool, types::CoreError>::failure(columns.error());
            }
            tableColumns.insert(tableName, columns.value());
        }

        const QString columnName = QString::fromLatin1(requirement.columnName);
        expectedColumns[tableName].insert(columnName);
        if (!tableColumns.value(tableName).contains(columnName))
        {
            return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
                QStringLiteral("Required catalog column %1.%2 is missing.").arg(tableName, columnName)));
        }
    }
    for (const QString& tableName : tableColumns.keys())
    {
        if (tableColumns.value(tableName) != expectedColumns.value(tableName))
        {
            return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
                QStringLiteral("Catalog table %1 has unsupported column extensions.").arg(tableName)));
        }
    }

    QHash<QString, QString> expectedPrimaryKeys;
    for (const CatalogKeyRequirement& requirement : RequiredCatalogPrimaryKeys)
    {
        if (schemaVersion < requirement.minimumVersion || schemaVersion > requirement.maximumVersion)
        {
            continue;
        }

        const QString tableName = QString::fromLatin1(requirement.tableName);
        const QString expectedKey = QString::fromLatin1(requirement.columns);
        const types::Result<QString, types::CoreError> actualKey = readCatalogPrimaryKey(database, tableName);
        if (actualKey.hasError())
        {
            return types::Result<bool, types::CoreError>::failure(actualKey.error());
        }
        if (actualKey.value() != expectedKey)
        {
            return types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog table %1 has an incompatible primary key.").arg(tableName)));
        }
        expectedPrimaryKeys.insert(tableName, expectedKey);
    }

    QHash<QString, QSet<QString>> expectedUniqueKeys;
    for (const CatalogKeyRequirement& requirement : RequiredCatalogUniqueKeys)
    {
        if (schemaVersion >= requirement.minimumVersion && schemaVersion <= requirement.maximumVersion)
        {
            expectedUniqueKeys[QString::fromLatin1(requirement.tableName)].insert(
                QString::fromLatin1(requirement.columns));
        }
    }
    for (const QString& tableName : tableColumns.keys())
    {
        const types::Result<QHash<QString, QString>, types::CoreError> actualUniqueKeys =
            readCatalogUniqueKeys(database, tableName);
        if (actualUniqueKeys.hasError())
        {
            return types::Result<bool, types::CoreError>::failure(actualUniqueKeys.error());
        }

        const QSet<QString> requiredUniqueKeys = expectedUniqueKeys.value(tableName);
        for (const QString& requiredKey : requiredUniqueKeys)
        {
            if (!actualUniqueKeys.value().contains(requiredKey) ||
                actualUniqueKeys.value().value(requiredKey) != QStringLiteral("u"))
            {
                return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
                    QStringLiteral("Catalog table %1 is missing required unique key %2.").arg(tableName, requiredKey)));
            }
        }
        for (auto actualKey = actualUniqueKeys.value().cbegin(); actualKey != actualUniqueKeys.value().cend();
             ++actualKey)
        {
            const bool requiredUniqueKey = requiredUniqueKeys.contains(actualKey.key());
            const bool requiredPrimaryKey =
                actualKey.value() == QStringLiteral("pk") && expectedPrimaryKeys.value(tableName) == actualKey.key();
            if (!requiredUniqueKey && !requiredPrimaryKey)
            {
                return types::Result<bool, types::CoreError>::failure(
                    makeDatabaseError(QStringLiteral("Catalog table %1 has an unsupported unique key %2.")
                                          .arg(tableName, actualKey.key())));
            }
        }
    }

    QHash<QString, QSet<QString>> expectedForeignKeys;
    for (const CatalogForeignKeyRequirement& requirement : RequiredCatalogForeignKeys)
    {
        if (schemaVersion >= requirement.minimumVersion && schemaVersion <= requirement.maximumVersion)
        {
            expectedForeignKeys[QString::fromLatin1(requirement.tableName)].insert(
                QString::fromLatin1(requirement.signature));
        }
    }
    for (const QString& tableName : tableColumns.keys())
    {
        const types::Result<QSet<QString>, types::CoreError> actualForeignKeys =
            readCatalogForeignKeys(database, tableName);
        if (actualForeignKeys.hasError())
        {
            return types::Result<bool, types::CoreError>::failure(actualForeignKeys.error());
        }
        if (actualForeignKeys.value() != expectedForeignKeys.value(tableName))
        {
            return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
                QStringLiteral("Catalog table %1 has incompatible foreign key semantics.").arg(tableName)));
        }
    }

    QSqlQuery triggerQuery(database);
    if (!triggerQuery.exec(QStringLiteral("SELECT name, tbl_name FROM sqlite_schema WHERE type = 'trigger'")))
    {
        return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to inspect catalog triggers: %1").arg(triggerQuery.lastError().text())));
    }
    while (triggerQuery.next())
    {
        const QString tableName = triggerQuery.value(1).toString();
        if (tableColumns.contains(tableName))
        {
            return types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog trigger %1 changes managed table %2 semantics.")
                                      .arg(triggerQuery.value(0).toString(), tableName)));
        }
    }

    return types::Result<bool, types::CoreError>::success(true);
}

// 목적: migration admission 또는 adoption 시 SQLite와 foreign key 무결성을 검증
// 입력: database: 검증할 열린 SQLite connection
// 출력: 무결성 충족 표식 또는 구조화된 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> verifyCatalogIntegrity(const QSqlDatabase& database)
{
    QSqlQuery integrityQuery(database);
    if (!integrityQuery.exec(QStringLiteral("PRAGMA integrity_check")) || !integrityQuery.next())
    {
        return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to run catalog integrity_check: %1").arg(integrityQuery.lastError().text())));
    }
    if (integrityQuery.value(0).toString() != QStringLiteral("ok") || integrityQuery.next())
    {
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog failed SQLite integrity_check.")));
    }

    QSqlQuery foreignKeyQuery(database);
    if (!foreignKeyQuery.exec(QStringLiteral("PRAGMA foreign_key_check")))
    {
        return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to run catalog foreign_key_check: %1").arg(foreignKeyQuery.lastError().text())));
    }
    if (foreignKeyQuery.next())
    {
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Catalog failed foreign_key_check at %1 row %2.")
                                  .arg(foreignKeyQuery.value(0).toString(), foreignKeyQuery.value(1).toString())));
    }

    return types::Result<bool, types::CoreError>::success(true);
}

// 목적: 실패했거나 검증을 마친 Qt SQLite connection을 안전하게 해제
// 입력: database: 해제할 Qt SQL handle, connectionName: Qt connection 식별자
// 출력: 없음
void removeConnection(QSqlDatabase& database, const QString& connectionName);

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

    if (!query.exec(QStringLiteral("SELECT version, typeof(version) FROM schema_version")))
    {
        return CatalogSchemaVersionResult::failure(makeDatabaseError(
            QStringLiteral("Unable to read the catalog schema version: %1").arg(query.lastError().text())));
    }
    if (!query.next())
    {
        return CatalogSchemaVersionResult::failure(
            makeDatabaseError(QStringLiteral("Catalog schema version row is missing.")));
    }

    bool converted = false;
    const int schemaVersion = query.value(0).toInt(&converted);
    if (!converted || query.value(1).toString() != QStringLiteral("integer") || schemaVersion <= 0 || query.next())
    {
        return CatalogSchemaVersionResult::failure(
            makeDatabaseError(QStringLiteral("Catalog schema version must be exactly one positive integer row.")));
    }
    return CatalogSchemaVersionResult::success(schemaVersion);
}

// 목적: schema version 전환용 Catalog backup의 고정 sidecar 경로 생성
// 입력: catalogPath: 원본 Catalog 경로, sourceVersion: 출발 version, targetVersion: 도착 version
// 출력: 원본명과 version 전환만 포함한 같은 directory의 backup 경로
[[nodiscard]] QString migrationBackupPath(const QString& catalogPath, int sourceVersion, int targetVersion)
{
    const QFileInfo catalogInfo(catalogPath);
    const QString backupFileName =
        QStringLiteral("%1.v%2-to-v%3.bak").arg(catalogInfo.fileName()).arg(sourceVersion).arg(targetVersion);
    return catalogInfo.dir().filePath(backupFileName);
}

// 목적: migration snapshot 내부에 rollback 출처 metadata 한 행 기록
// 입력: backupPath: snapshot, catalogPath: 원본 경로, catalogUuid: 원본 identity, version: 전환 범위
// 출력: metadata 기록 성공 표식 또는 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> writeMigrationBackupMetadata(const QString& backupPath,
                                                                                 const QString& catalogPath,
                                                                                 const QString& catalogUuid,
                                                                                 int sourceVersion,
                                                                                 int targetVersion)
{
    const QString connectionName =
        QStringLiteral("catalog_backup_write_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setDatabaseName(backupPath);
    types::Result<bool, types::CoreError> result = types::Result<bool, types::CoreError>::success(false);

    if (!database.open())
    {
        result = types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to open the catalog migration backup: %1").arg(database.lastError().text())));
    }
    else if (!database.transaction())
    {
        result = types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to begin catalog migration backup metadata: %1").arg(database.lastError().text())));
    }
    else
    {
        QSqlQuery query(database);
        const QString resetStatement =
            QStringLiteral("DROP TABLE IF EXISTS %1").arg(QString::fromLatin1(MigrationBackupMetadataTable));
        const QString createStatement = QStringLiteral("CREATE TABLE %1 ("
                                                       "id INTEGER PRIMARY KEY CHECK (id = 1), "
                                                       "source_version INTEGER NOT NULL, "
                                                       "target_version INTEGER NOT NULL, "
                                                       "created_at_ms INTEGER NOT NULL, "
                                                       "application_version TEXT NOT NULL, "
                                                       "original_file_name TEXT NOT NULL, "
                                                       "catalog_uuid TEXT NOT NULL)")
                                            .arg(QString::fromLatin1(MigrationBackupMetadataTable));
        const QString insertStatement =
            QStringLiteral("INSERT INTO %1 (id, source_version, target_version, created_at_ms, "
                           "application_version, original_file_name, catalog_uuid) "
                           "VALUES (1, :sourceVersion, :targetVersion, :createdAtMs, :applicationVersion, "
                           ":originalFileName, :catalogUuid)")
                .arg(QString::fromLatin1(MigrationBackupMetadataTable));

        if (!query.exec(resetStatement) || !query.exec(createStatement) || !query.prepare(insertStatement))
        {
            database.rollback();
            result = types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Unable to prepare catalog migration backup metadata: %1")
                                      .arg(query.lastError().text())));
        }
        else
        {
            query.bindValue(QStringLiteral(":sourceVersion"), sourceVersion);
            query.bindValue(QStringLiteral(":targetVersion"), targetVersion);
            query.bindValue(QStringLiteral(":createdAtMs"), QDateTime::currentMSecsSinceEpoch());
            query.bindValue(QStringLiteral(":applicationVersion"), QStringLiteral(FLEXRAW_VERSION_STRING));
            query.bindValue(QStringLiteral(":originalFileName"), QFileInfo(catalogPath).fileName());
            query.bindValue(QStringLiteral(":catalogUuid"), catalogUuid);
            if (!query.exec() || !database.commit())
            {
                database.rollback();
                result = types::Result<bool, types::CoreError>::failure(
                    makeDatabaseError(QStringLiteral("Unable to write catalog migration backup metadata: %1")
                                          .arg(query.lastError().text().isEmpty() ? database.lastError().text()
                                                                                  : query.lastError().text())));
            }
            else
            {
                result = types::Result<bool, types::CoreError>::success(true);
            }
        }
    }

    removeConnection(database, connectionName);
    return result;
}

// 목적: migration backup의 SQLite 일관성과 version 전환 metadata 검증
// 입력: backupPath: backup, catalogPath: 원본 경로, catalogUuid: 원본 identity, version: 기대 전환 범위
// 출력: 검증 성공 표식 또는 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> verifyMigrationBackup(const QString& backupPath,
                                                                          const QString& catalogPath,
                                                                          const QString& catalogUuid,
                                                                          int sourceVersion,
                                                                          int targetVersion)
{
    const QString connectionName =
        QStringLiteral("catalog_backup_verify_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    database.setDatabaseName(backupPath);
    types::Result<bool, types::CoreError> result = types::Result<bool, types::CoreError>::success(false);

    if (!database.open())
    {
        result = types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to open the catalog migration backup for verification: %1")
                                  .arg(database.lastError().text())));
    }
    else
    {
        const CatalogSchemaVersionResult version = readSchemaVersion(database);
        QSqlQuery integrityQuery(database);
        QSqlQuery foreignKeyQuery(database);
        QSqlQuery metadataQuery(database);
        const QString metadataStatement =
            QStringLiteral("SELECT source_version, target_version, created_at_ms, application_version, "
                           "original_file_name, catalog_uuid FROM %1 WHERE id = 1")
                .arg(QString::fromLatin1(MigrationBackupMetadataTable));
        const CatalogIdentityResult backupIdentity = readCatalogIdentity(database);

        if (version.hasError() || version.value() != sourceVersion || backupIdentity.hasError() ||
            backupIdentity.value() != catalogUuid)
        {
            result = types::Result<bool, types::CoreError>::failure(makeDatabaseError(
                QStringLiteral("Catalog migration backup identity or schema version does not match its source.")));
        }
        else if (!integrityQuery.exec(QStringLiteral("PRAGMA integrity_check")) || !integrityQuery.next() ||
                 integrityQuery.value(0).toString() != QStringLiteral("ok") || integrityQuery.next())
        {
            result = types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog migration backup failed SQLite integrity_check.")));
        }
        else if (!foreignKeyQuery.exec(QStringLiteral("PRAGMA foreign_key_check")) || foreignKeyQuery.next())
        {
            result = types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog migration backup failed foreign_key_check.")));
        }
        else if (!metadataQuery.exec(metadataStatement) || !metadataQuery.next())
        {
            result = types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog migration backup metadata is missing or invalid: %1")
                                      .arg(metadataQuery.lastError().text())));
        }
        else if (metadataQuery.value(0).toInt() != sourceVersion || metadataQuery.value(1).toInt() != targetVersion ||
                 metadataQuery.value(2).toLongLong() <= 0 || metadataQuery.value(3).toString().isEmpty() ||
                 metadataQuery.value(4).toString() != QFileInfo(catalogPath).fileName() ||
                 metadataQuery.value(5).toString() != catalogUuid || metadataQuery.next())
        {
            result = types::Result<bool, types::CoreError>::failure(
                makeDatabaseError(QStringLiteral("Catalog migration backup metadata does not match this transition.")));
        }
        else
        {
            result = types::Result<bool, types::CoreError>::success(true);
        }
    }

    removeConnection(database, connectionName);
    return result;
}

// 목적: 구버전 Catalog를 변경하기 전에 검증 가능한 같은-directory snapshot 보장
// 입력: database: 원본 connection, catalogPath: 원본 경로, catalogUuid: 원본 identity, sourceVersion: 현재 version
// 출력: backup 준비 성공 표식 또는 migration을 중단할 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> ensureMigrationBackup(QSqlDatabase& database,
                                                                          const QString& catalogPath,
                                                                          const QString& catalogUuid,
                                                                          int sourceVersion)
{
    if (sourceVersion <= 0 || sourceVersion >= CurrentSchemaVersion)
    {
        return types::Result<bool, types::CoreError>::success(true);
    }

    const QString backupPath = migrationBackupPath(catalogPath, sourceVersion, CurrentSchemaVersion);
    if (QFileInfo::exists(backupPath))
    {
        return verifyMigrationBackup(backupPath, catalogPath, catalogUuid, sourceVersion, CurrentSchemaVersion);
    }

    const QString temporaryPath =
        QStringLiteral("%1.tmp-%2").arg(backupPath, QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSqlQuery snapshotQuery(database);
    snapshotQuery.prepare(QStringLiteral("VACUUM INTO :backupPath"));
    snapshotQuery.bindValue(QStringLiteral(":backupPath"), temporaryPath);
    if (!snapshotQuery.exec())
    {
        QFile::remove(temporaryPath);
        return types::Result<bool, types::CoreError>::failure(makeDatabaseError(
            QStringLiteral("Unable to create the catalog migration backup: %1").arg(snapshotQuery.lastError().text())));
    }

    const types::Result<bool, types::CoreError> metadata =
        writeMigrationBackupMetadata(temporaryPath, catalogPath, catalogUuid, sourceVersion, CurrentSchemaVersion);
    if (metadata.hasError())
    {
        QFile::remove(temporaryPath);
        return metadata;
    }

    const types::Result<bool, types::CoreError> verified =
        verifyMigrationBackup(temporaryPath, catalogPath, catalogUuid, sourceVersion, CurrentSchemaVersion);
    if (verified.hasError())
    {
        QFile::remove(temporaryPath);
        return verified;
    }

    if (!QFile::rename(temporaryPath, backupPath))
    {
        QFile::remove(temporaryPath);
        return types::Result<bool, types::CoreError>::failure(
            makeDatabaseError(QStringLiteral("Unable to commit the catalog migration backup beside the catalog.")));
    }

    return types::Result<bool, types::CoreError>::success(true);
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

    QLockFile migrationLock(validatedPath.value() + QStringLiteral(".migration.lock"));
    migrationLock.setStaleLockTime(0);
    if (!migrationLock.tryLock(0))
    {
        const types::ErrorCode errorCode = migrationLock.error() == QLockFile::PermissionError
                                               ? types::ErrorCode::PermissionDenied
                                               : types::ErrorCode::Conflict;
        return CatalogDatabaseOpenResult::failure(
            {errorCode, QStringLiteral("Another process owns the Catalog startup and migration lock.")});
    }

    const bool catalogExisted = QFileInfo::exists(validatedPath.value());
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

    const types::Result<int, types::CoreError> applicationId = readCatalogApplicationId(database);
    if (applicationId.hasError())
    {
        const types::CoreError error = applicationId.error();
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }
    if (applicationId.value() != 0 && applicationId.value() != CatalogApplicationId)
    {
        const types::CoreError error =
            makeDatabaseError(QStringLiteral("The SQLite file belongs to another application format."));
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }

    const CatalogSchemaVersionResult sourceVersion = readSchemaVersion(database);
    if (sourceVersion.hasError())
    {
        const types::CoreError error = sourceVersion.error();
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }
    if (catalogExisted && sourceVersion.value() == 0)
    {
        const types::CoreError error =
            makeDatabaseError(QStringLiteral("The existing SQLite file is not a recognized FlexRAW catalog."));
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }
    if (sourceVersion.value() > CurrentSchemaVersion)
    {
        const types::CoreError error =
            makeDatabaseError(QStringLiteral("The catalog schema version is newer than this application supports."));
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }

    if (sourceVersion.value() > 0)
    {
        const types::Result<bool, types::CoreError> sourceSchema =
            validateRequiredCatalogSchema(database, sourceVersion.value());
        if (sourceSchema.hasError())
        {
            const types::CoreError error = sourceSchema.error();
            removeConnection(database, connectionName);
            return CatalogDatabaseOpenResult::failure(error);
        }
    }

    const bool requiresMigration = sourceVersion.value() > 0 && sourceVersion.value() < CurrentSchemaVersion;
    const bool requiresLegacyAdoption = catalogExisted && applicationId.value() == 0;
    if (requiresMigration || requiresLegacyAdoption)
    {
        const types::Result<bool, types::CoreError> sourceIntegrity = verifyCatalogIntegrity(database);
        if (sourceIntegrity.hasError())
        {
            const types::CoreError error = sourceIntegrity.error();
            removeConnection(database, connectionName);
            return CatalogDatabaseOpenResult::failure(error);
        }
    }

    QString catalogUuid;
    if (sourceVersion.value() > 0)
    {
        const CatalogIdentityResult identity = ensureCatalogIdentity(database);
        if (identity.hasError())
        {
            const types::CoreError error = identity.error();
            removeConnection(database, connectionName);
            return CatalogDatabaseOpenResult::failure(error);
        }
        catalogUuid = identity.value();
    }

    const types::Result<bool, types::CoreError> backup =
        ensureMigrationBackup(database, validatedPath.value(), catalogUuid, sourceVersion.value());
    if (backup.hasError())
    {
        const types::CoreError error = backup.error();
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

    const types::Result<bool, types::CoreError> currentSchema =
        validateRequiredCatalogSchema(database, CurrentSchemaVersion);
    if (currentSchema.hasError())
    {
        const types::CoreError error = currentSchema.error();
        removeConnection(database, connectionName);
        return CatalogDatabaseOpenResult::failure(error);
    }

    const CatalogIdentityResult currentIdentity = readCatalogIdentity(database);
    if (currentIdentity.hasError() || (!catalogUuid.isEmpty() && currentIdentity.value() != catalogUuid))
    {
        const types::CoreError error =
            currentIdentity.hasError()
                ? currentIdentity.error()
                : makeDatabaseError(QStringLiteral("Catalog identity changed during migration."));
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

    if (!catalogExisted || requiresMigration || requiresLegacyAdoption)
    {
        const types::Result<bool, types::CoreError> currentIntegrity = verifyCatalogIntegrity(database);
        if (currentIntegrity.hasError())
        {
            const types::CoreError error = currentIntegrity.error();
            removeConnection(database, connectionName);
            return CatalogDatabaseOpenResult::failure(error);
        }
    }

    if (applicationId.value() == 0)
    {
        const types::Result<bool, types::CoreError> applicationIdWritten = writeCatalogApplicationId(database);
        if (applicationIdWritten.hasError())
        {
            const types::CoreError error = applicationIdWritten.error();
            removeConnection(database, connectionName);
            return CatalogDatabaseOpenResult::failure(error);
        }
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

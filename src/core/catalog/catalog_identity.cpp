#include "catalog_identity.h"

#include <utility>

#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>

namespace flexraw::core::catalog
{
namespace
{

constexpr auto CatalogMetadataTable = "flexraw_catalog_metadata";

// 목적: Catalog identity 관련 실패 정보를 일관된 CoreError로 생성
// 입력: message: 호출자에게 전달할 database 오류 설명
// 출력: DatabaseError로 분류된 CoreError 객체
[[nodiscard]] types::CoreError makeIdentityError(QString message)
{
    return {types::ErrorCode::DatabaseError, std::move(message)};
}

// 목적: UUID text를 FlexRAW Catalog identity canonical form으로 검증
// 입력: value: SQLite row에서 읽은 UUID text
// 출력: 소문자 하이픈 UUID 또는 빈 문자열
[[nodiscard]] QString normalizedCatalogUuid(const QString& value)
{
    const QUuid uuid(value);
    if (uuid.isNull())
    {
        return {};
    }

    const QString normalized = uuid.toString(QUuid::WithoutBraces).toLower();
    return value == normalized ? normalized : QString{};
}

// 목적: Catalog identity table의 column과 key 구성이 기대 계약과 같은지 검증
// 입력: database: identity table을 보유한 SQLite connection
// 출력: 구조 계약 충족 표식 또는 구조화된 database 오류
[[nodiscard]] types::Result<bool, types::CoreError> validateIdentityTableSchema(const QSqlDatabase& database)
{
    const QString tableName = QString::fromLatin1(CatalogMetadataTable);
    QSqlQuery columnQuery(database);
    if (!columnQuery.exec(QStringLiteral("PRAGMA table_info(%1)").arg(tableName)))
    {
        return types::Result<bool, types::CoreError>::failure(makeIdentityError(
            QStringLiteral("Unable to inspect the catalog identity table: %1").arg(columnQuery.lastError().text())));
    }

    QSet<QString> columns;
    QStringList primaryKeyColumns;
    while (columnQuery.next())
    {
        columns.insert(columnQuery.value(1).toString());
        if (columnQuery.value(5).toInt() > 0)
        {
            primaryKeyColumns.append(columnQuery.value(1).toString());
        }
    }
    if (columns != QSet<QString>{QStringLiteral("id"), QStringLiteral("catalog_uuid")} ||
        primaryKeyColumns != QStringList{QStringLiteral("id")})
    {
        return types::Result<bool, types::CoreError>::failure(
            makeIdentityError(QStringLiteral("Catalog identity table has an incompatible schema.")));
    }

    QSqlQuery indexListQuery(database);
    if (!indexListQuery.exec(QStringLiteral("PRAGMA index_list(%1)").arg(tableName)))
    {
        return types::Result<bool, types::CoreError>::failure(makeIdentityError(
            QStringLiteral("Unable to inspect the catalog identity keys: %1").arg(indexListQuery.lastError().text())));
    }

    bool hasCatalogUuidUniqueKey = false;
    while (indexListQuery.next())
    {
        if (indexListQuery.value(2).toInt() == 0)
        {
            continue;
        }

        QString escapedIndexName = indexListQuery.value(1).toString();
        escapedIndexName.replace(QLatin1Char('\''), QStringLiteral("''"));
        QSqlQuery indexInfoQuery(database);
        if (!indexInfoQuery.exec(QStringLiteral("PRAGMA index_info('%1')").arg(escapedIndexName)))
        {
            return types::Result<bool, types::CoreError>::failure(makeIdentityError(
                QStringLiteral("Unable to inspect a catalog identity key: %1").arg(indexInfoQuery.lastError().text())));
        }

        QStringList indexColumns;
        while (indexInfoQuery.next())
        {
            indexColumns.append(indexInfoQuery.value(2).toString());
        }
        if (indexColumns == QStringList{QStringLiteral("catalog_uuid")})
        {
            hasCatalogUuidUniqueKey = true;
        }
        else if (indexColumns != QStringList{QStringLiteral("id")})
        {
            return types::Result<bool, types::CoreError>::failure(
                makeIdentityError(QStringLiteral("Catalog identity table has an unsupported unique key.")));
        }
    }
    if (!hasCatalogUuidUniqueKey)
    {
        return types::Result<bool, types::CoreError>::failure(
            makeIdentityError(QStringLiteral("Catalog identity table is missing its UUID unique key.")));
    }

    QSqlQuery foreignKeyQuery(database);
    if (!foreignKeyQuery.exec(QStringLiteral("PRAGMA foreign_key_list(%1)").arg(tableName)) || foreignKeyQuery.next())
    {
        return types::Result<bool, types::CoreError>::failure(
            makeIdentityError(QStringLiteral("Catalog identity table has unsupported foreign key semantics.")));
    }

    QSqlQuery triggerQuery(database);
    triggerQuery.prepare(QStringLiteral("SELECT 1 FROM sqlite_schema WHERE type = 'trigger' AND tbl_name = :table"));
    triggerQuery.bindValue(QStringLiteral(":table"), tableName);
    if (!triggerQuery.exec() || triggerQuery.next())
    {
        return types::Result<bool, types::CoreError>::failure(
            makeIdentityError(QStringLiteral("Catalog identity table has unsupported trigger semantics.")));
    }

    return types::Result<bool, types::CoreError>::success(true);
}

}  // namespace

CatalogIdentityResult readCatalogIdentity(const QSqlDatabase& database)
{
    const QString tableName = QString::fromLatin1(CatalogMetadataTable);
    if (!database.tables().contains(tableName))
    {
        return CatalogIdentityResult::failure(makeIdentityError(QStringLiteral("Catalog identity table is missing.")));
    }

    const types::Result<bool, types::CoreError> schema = validateIdentityTableSchema(database);
    if (schema.hasError())
    {
        return CatalogIdentityResult::failure(schema.error());
    }

    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT id, catalog_uuid, typeof(catalog_uuid) FROM %1").arg(tableName)) ||
        !query.next())
    {
        return CatalogIdentityResult::failure(
            makeIdentityError(QStringLiteral("Unable to read the catalog identity: %1").arg(query.lastError().text())));
    }

    const QString catalogUuid = normalizedCatalogUuid(query.value(1).toString());
    if (query.value(0).toInt() != 1 || query.value(2).toString() != QStringLiteral("text") || catalogUuid.isEmpty() ||
        query.next())
    {
        return CatalogIdentityResult::failure(
            makeIdentityError(QStringLiteral("Catalog identity must be exactly one canonical UUID row.")));
    }
    return CatalogIdentityResult::success(catalogUuid);
}

CatalogIdentityResult ensureCatalogIdentity(QSqlDatabase& database)
{
    const QString tableName = QString::fromLatin1(CatalogMetadataTable);
    if (database.tables().contains(tableName))
    {
        return readCatalogIdentity(database);
    }

    if (!database.transaction())
    {
        return CatalogIdentityResult::failure(makeIdentityError(
            QStringLiteral("Unable to begin catalog identity adoption: %1").arg(database.lastError().text())));
    }

    const QString catalogUuid = QUuid::createUuid().toString(QUuid::WithoutBraces).toLower();
    QSqlQuery query(database);
    const QString createStatement =
        QStringLiteral("CREATE TABLE %1 ("
                       "id INTEGER PRIMARY KEY CHECK (id = 1), "
                       "catalog_uuid TEXT NOT NULL UNIQUE CHECK (length(catalog_uuid) = 36))")
            .arg(tableName);
    const QString insertStatement =
        QStringLiteral("INSERT INTO %1 (id, catalog_uuid) VALUES (1, :catalogUuid)").arg(tableName);
    if (!query.exec(createStatement) || !query.prepare(insertStatement))
    {
        database.rollback();
        return CatalogIdentityResult::failure(makeIdentityError(
            QStringLiteral("Unable to prepare catalog identity adoption: %1").arg(query.lastError().text())));
    }

    query.bindValue(QStringLiteral(":catalogUuid"), catalogUuid);
    if (!query.exec() || !database.commit())
    {
        database.rollback();
        return CatalogIdentityResult::failure(makeIdentityError(
            QStringLiteral("Unable to persist the catalog identity: %1").arg(query.lastError().text())));
    }

    return readCatalogIdentity(database);
}

}  // namespace flexraw::core::catalog

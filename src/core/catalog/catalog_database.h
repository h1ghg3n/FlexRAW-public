#pragma once

#include <memory>

#include <QSqlDatabase>
#include <QString>

#include "error.h"
#include "result.h"

namespace flexraw::core::catalog
{

class CatalogDatabase;
class CatalogDevelopRepository;
class CatalogPhotoRepository;
class CatalogProjectRepository;
class CatalogPresetRepository;

using CatalogDatabasePtr = std::unique_ptr<CatalogDatabase>;
using CatalogDatabaseOpenResult = types::Result<CatalogDatabasePtr, types::CoreError>;
using CatalogSchemaVersionResult = types::Result<int, types::CoreError>;

class CatalogDatabase final
{
public:
    // 목적: catalog SQLite database를 열고 필요한 migration을 적용
    // 입력: catalogPath: 생성 또는 열 database file 경로
    // 출력: 열린 CatalogDatabase 또는 구조화된 오류
    [[nodiscard]] static CatalogDatabaseOpenResult open(const QString& catalogPath);

    // 목적: database connection과 연관된 Qt resource를 정리
    // 입력: 없음
    // 출력: 없음
    ~CatalogDatabase();

    CatalogDatabase(const CatalogDatabase&) = delete;
    CatalogDatabase& operator=(const CatalogDatabase&) = delete;

    // 목적: 현재 적용된 catalog schema version을 조회
    // 입력: 없음
    // 출력: schema version 또는 구조화된 database 오류
    [[nodiscard]] CatalogSchemaVersionResult schemaVersion() const;

    // 목적: 열린 catalog database file의 정규화된 경로 반환
    // 입력: 없음
    // 출력: catalog database 절대 경로
    [[nodiscard]] const QString& catalogPath() const noexcept;

private:
    friend class CatalogDevelopRepository;
    friend class CatalogPhotoRepository;
    friend class CatalogProjectRepository;
    friend class CatalogPresetRepository;

    // 목적: 열린 Qt SQL connection과 경로를 보관하는 CatalogDatabase 생성
    // 입력: database: 열린 SQLite connection, connectionName: Qt connection 식별자, catalogPath: database 경로
    // 출력: 초기화된 CatalogDatabase 객체
    CatalogDatabase(QSqlDatabase database, QString connectionName, QString catalogPath);

    QSqlDatabase m_database;
    QString m_connectionName;
    QString m_catalogPath;
};

}  // namespace flexraw::core::catalog

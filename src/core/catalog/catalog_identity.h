#pragma once

#include <QString>

#include "error.h"
#include "result.h"

class QSqlDatabase;

namespace flexraw::core::catalog
{

using CatalogIdentityResult = types::Result<QString, types::CoreError>;

// 목적: 기존 Catalog의 영속 identity를 검증하거나 legacy Catalog에 새 identity를 부여
// 입력: database: 열린 SQLite connection
// 출력: 정규화된 Catalog UUID 또는 구조화된 database 오류
[[nodiscard]] CatalogIdentityResult ensureCatalogIdentity(QSqlDatabase& database);

// 목적: Catalog에 기록된 영속 identity 구조와 단일 UUID row를 검증해 조회
// 입력: database: 열린 SQLite connection
// 출력: 정규화된 Catalog UUID 또는 구조화된 database 오류
[[nodiscard]] CatalogIdentityResult readCatalogIdentity(const QSqlDatabase& database);

}  // namespace flexraw::core::catalog

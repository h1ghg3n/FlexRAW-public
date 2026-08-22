#pragma once

#include "catalog_entry.h"
#include "error.h"
#include "result.h"

#include <QString>
#include <QVector>

namespace flexraw::core::catalog {

using CatalogScanResult = types::Result<QVector<CatalogEntry>, types::CoreError>;

// 목적: 지정된 folder 에서 지원되는 RAW/image 파일 목록 검색
// 입력: folderPath: scan 대상 folder 경로
// 출력: CatalogEntry 목록 또는 구조화된 오류
[[nodiscard]] CatalogScanResult scanFolder(const QString& folderPath);

} // namespace flexraw::core::catalog

#pragma once

#include "catalog_database.h"
#include "catalog_entry.h"
#include "catalog_photo_repository.h"
#include "result.h"

#include <QVector>

namespace flexraw::core::catalog {

struct CatalogFolderImportReport {
    int storedCount{0};
    QVector<CatalogEntry> entries;
};

using CatalogFolderImportResult = types::Result<CatalogFolderImportReport, types::CoreError>;

class CatalogFolderImporter final
{
public:
    // 목적: 열린 catalog database에 사진 folder importer 연결
    // 입력: database: photos table을 보유한 열린 CatalogDatabase
    // 출력: 초기화된 CatalogFolderImporter 객체
    explicit CatalogFolderImporter(CatalogDatabase& database);

    // 목적: 지정된 folder를 scan하고 지원 사진을 catalog에 저장
    // 입력: folderPath: import할 사진 folder 경로
    // 출력: 저장 수와 scan 결과 또는 구조화된 오류
    [[nodiscard]] CatalogFolderImportResult importFolder(const QString& folderPath);

private:
    CatalogPhotoRepository m_photoRepository;
};

} // namespace flexraw::core::catalog

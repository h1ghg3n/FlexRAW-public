#include "catalog_folder_importer.h"

#include "folder_scanner.h"

#include <utility>

namespace flexraw::core::catalog {

CatalogFolderImporter::CatalogFolderImporter(CatalogDatabase& database)
    : m_photoRepository(database)
{
}

CatalogFolderImportResult CatalogFolderImporter::importFolder(const QString& folderPath)
{
    CatalogScanResult scannedEntries = scanFolder(folderPath);

    if (scannedEntries.hasError()) {
        return CatalogFolderImportResult::failure(scannedEntries.error());
    }

    QVector<CatalogEntry> entries = std::move(scannedEntries.value());
    const CatalogPhotoStoreResult stored = m_photoRepository.upsert(entries);

    if (stored.hasError()) {
        return CatalogFolderImportResult::failure(stored.error());
    }

    return CatalogFolderImportResult::success({stored.value(), std::move(entries)});
}

} // namespace flexraw::core::catalog

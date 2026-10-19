#pragma once

#include <optional>

#include <QString>
#include <QVector>

#include "catalog_culling.h"
#include "catalog_entry.h"
#include "catalog_inclusion.h"
#include "catalog_project.h"

namespace flexraw::core::catalog
{

inline constexpr int DefaultCatalogPhotoPageSize = 100;
inline constexpr int MaximumCatalogPhotoPageSize = 200;

enum class CatalogPhotoPageDirection
{
    Forward,
    Backward,
};

struct CatalogPhotoPageCursor
{
    QString displayName;
    types::PhotoId photoId;
    std::optional<QString> exactFolderPath;
    std::optional<ProjectId> projectId;
    CatalogCullingFilter cullingFilter{CatalogCullingFilter::All};
    CatalogInclusionFilter inclusionFilter{CatalogInclusionFilter::IncludedOnly};
};

struct CatalogPhotoPageRequest
{
    int pageSize{DefaultCatalogPhotoPageSize};
    CatalogPhotoPageDirection direction{CatalogPhotoPageDirection::Forward};
    std::optional<CatalogPhotoPageCursor> cursor;
    std::optional<QString> exactFolderPath;
    std::optional<ProjectId> projectId;
    CatalogCullingFilter cullingFilter{CatalogCullingFilter::All};
    CatalogInclusionFilter inclusionFilter{CatalogInclusionFilter::IncludedOnly};
};

struct CatalogPhotoPage
{
    QVector<CatalogPhotoRecord> photos;
    std::optional<CatalogPhotoPageCursor> previousCursor;
    std::optional<CatalogPhotoPageCursor> nextCursor;
};

}  // namespace flexraw::core::catalog

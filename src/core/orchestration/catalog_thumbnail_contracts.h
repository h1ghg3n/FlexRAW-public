#pragma once

#include <variant>

#include <QImage>
#include <QMetaType>
#include <QSize>
#include <QString>
#include <QVector>

#include "error.h"
#include "file_types.h"
#include "result.h"

namespace flexraw::core::orchestration
{

inline constexpr int MaximumCatalogThumbnailWindowSize = 200;

struct CatalogThumbnailWindowRequest
{
    QVector<types::FileDescriptor> sources;
    QSize targetSize;
};

struct CatalogThumbnailFrame
{
    QString sourcePath;
    QImage image;
};

struct CatalogThumbnailIssue
{
    QString sourcePath;
    types::CoreError error;
};

using CatalogThumbnailWindowResult = types::Result<std::monostate, types::CoreError>;

}  // namespace flexraw::core::orchestration

Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogThumbnailFrame)
Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogThumbnailIssue)

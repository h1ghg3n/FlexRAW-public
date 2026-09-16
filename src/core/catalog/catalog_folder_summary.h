#pragma once

#include <QString>
#include <QtGlobal>

namespace flexraw::core::catalog
{

struct CatalogFolderSummary
{
    QString path;
    qint64 photoCount{0};
};

}  // namespace flexraw::core::catalog

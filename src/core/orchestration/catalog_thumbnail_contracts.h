#pragma once

#include <optional>

#include <QImage>
#include <QMetaType>

#include "catalog_thumbnail_client.h"
#include "error.h"

namespace flexraw::core::orchestration
{

struct CatalogThumbnailWindowStarted
{
    client::CatalogThumbnailWindowReceipt receipt;
    client::CatalogThumbnailTargetExtent targetExtent;
};

struct CatalogThumbnailFrame
{
    client::CatalogThumbnailWindowGeneration generation;
    client::CatalogThumbnailItemIdentity identity;
    QImage image;
};

struct CatalogThumbnailIssue
{
    client::CatalogThumbnailWindowGeneration generation;
    client::CatalogThumbnailItemIdentity identity;
    types::CoreError error;
};

struct CatalogThumbnailWindowTerminal
{
    client::CatalogThumbnailWindowGeneration generation;
    client::CatalogThumbnailTerminalState state{client::CatalogThumbnailTerminalState::Completed};
    std::optional<types::CoreError> error;
};

}  // namespace flexraw::core::orchestration

Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogThumbnailWindowStarted)
Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogThumbnailFrame)
Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogThumbnailIssue)
Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogThumbnailWindowTerminal)

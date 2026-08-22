#pragma once

#include <optional>

#include <QMetaType>
#include <QString>
#include <QVector>

#include "catalog_entry.h"
#include "catalog_photo_page.h"
#include "develop_params.h"
#include "error.h"
#include "operation_types.h"
#include "result.h"

namespace flexraw::core::orchestration
{

struct CatalogSessionState
{
    bool isOpen{false};
    QString catalogPath;
};

struct CatalogPhotoState
{
    catalog::CatalogPhotoRecord photo;
    types::DevelopParams developParams;
    types::DevelopRevision persistedRevision{0};
    bool sourceProcessingAllowed{false};
    std::optional<types::RequestId> sourceVerificationRequestId;
};

struct CatalogImportSummary
{
    int scannedCount{0};
    int storedCount{0};
    QVector<types::PhotoId> photoIds;
    QVector<types::RequestId> fingerprintRequestIds;
};

struct CatalogSourceUpdate
{
    types::RequestId requestId{0};
    types::PhotoId photoId;
    catalog::CatalogPhotoRecord photo;
    std::optional<catalog::CatalogPhotoRecord> createdPhoto;
};

struct CatalogIssue
{
    types::RequestId requestId{0};
    types::PhotoId photoId;
    types::CoreError error;
};

using CatalogSessionResult = types::Result<CatalogSessionState, types::CoreError>;
using CatalogPhotoPageResult = types::Result<catalog::CatalogPhotoPage, types::CoreError>;
using CatalogPhotoStateResult = types::Result<CatalogPhotoState, types::CoreError>;
using CatalogPhotoRegistrationResult = types::Result<types::PhotoId, types::CoreError>;
using CatalogImportResult = types::Result<CatalogImportSummary, types::CoreError>;
using CatalogSourceSubmissionResult = types::Result<types::RequestId, types::CoreError>;

}  // namespace flexraw::core::orchestration

Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogPhotoState)
Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogSourceUpdate)
Q_DECLARE_METATYPE(flexraw::core::orchestration::CatalogIssue)

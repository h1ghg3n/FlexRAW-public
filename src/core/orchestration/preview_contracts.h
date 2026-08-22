#pragma once

#include <QImage>
#include <QMetaType>
#include <QSize>
#include <QString>

#include "clipping.h"
#include "develop_params.h"
#include "error.h"
#include "file_types.h"
#include "histogram.h"
#include "operation_types.h"
#include "photo_identity.h"
#include "result.h"

namespace flexraw::core::orchestration
{

enum class PreviewTier
{
    Thumbnail,
    Standard,
};

enum class PreviewProgression
{
    Progressive,
    FinalOnly,
};

enum class PreviewRenderMode
{
    Interactive,
    Final,
};

struct PhotoSnapshot
{
    types::PhotoId photoId;
    QString transientKey;
    types::DevelopRevision developRevision{0};
};

struct PreviewOperationStats
{
    qint64 queueWaitMs{0};
    qint64 cacheReadMs{0};
    qint64 decodeMs{0};
    qint64 cacheWriteMs{0};
    qint64 developMs{0};
    qint64 analysisMs{0};
    qint64 totalMs{0};
    bool sourceCacheHit{false};
};

struct PreviewRequest
{
    PhotoSnapshot photo;
    types::FileDescriptor source;
    QSize targetSize;
    types::DevelopParams params;
    types::PreviewSequence previewSequence{0};
    PreviewProgression progression{PreviewProgression::Progressive};
    PreviewRenderMode renderMode{PreviewRenderMode::Final};
};

struct PreviewResult
{
    types::RequestId requestId{0};
    PhotoSnapshot photo;
    QString sourcePath;
    types::PreviewSequence previewSequence{0};
    PreviewTier tier{PreviewTier::Thumbnail};
    PreviewRenderMode renderMode{PreviewRenderMode::Final};
    QImage image;
    develop::ImageHistogram histogram;
    develop::ClippingSummary clipping;
    PreviewOperationStats stats;
};

struct PreviewIssue
{
    types::RequestId requestId{0};
    PhotoSnapshot photo;
    QString sourcePath;
    types::PreviewSequence previewSequence{0};
    types::CoreError error;
};

using PreviewSubmissionResult = types::Result<types::RequestId, types::CoreError>;

}  // namespace flexraw::core::orchestration

Q_DECLARE_METATYPE(flexraw::core::orchestration::PreviewTier)
Q_DECLARE_METATYPE(flexraw::core::orchestration::PreviewProgression)
Q_DECLARE_METATYPE(flexraw::core::orchestration::PreviewRenderMode)
Q_DECLARE_METATYPE(flexraw::core::orchestration::PreviewResult)
Q_DECLARE_METATYPE(flexraw::core::orchestration::PreviewIssue)

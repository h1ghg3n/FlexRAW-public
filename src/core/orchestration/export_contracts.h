#pragma once

#include <cstdint>
#include <optional>
#include <variant>

#include <QMetaType>
#include <QString>
#include <QVector>

#include "develop_params.h"
#include "error.h"
#include "export_options.h"
#include "file_types.h"
#include "operation_types.h"
#include "result.h"

namespace flexraw::core::orchestration
{

enum class ExportPlacementPolicy : std::uint8_t
{
    LocalOnly,
    RemoteOnly,
    Auto,
};

struct ExportRemoteTarget
{
    QString host;
    std::uint16_t port{47331};
    QString expectedSourceStorageId;
    QString expectedOutputStorageId;
};

struct ExportPlacementOptions
{
    ExportPlacementPolicy policy{ExportPlacementPolicy::LocalOnly};
    std::optional<ExportRemoteTarget> remoteTarget;
};

enum class ExportItemFailureKind : std::uint8_t
{
    None,
    Eligibility,
    DispatchExhausted,
    Execution,
    Ambiguous,
};

struct ExportSchedulingMetrics
{
    std::uint64_t submitted{0};
    std::uint64_t succeeded{0};
    std::uint64_t failed{0};
    std::uint64_t cancelled{0};
    std::uint64_t ambiguous{0};
    std::uint64_t localDispatchAttempts{0};
    std::uint64_t remoteDispatchAttempts{0};
    std::uint64_t serverBusyCount{0};
    std::uint64_t resourceBusyCount{0};
    std::uint64_t connectionFailedCount{0};
    std::uint64_t localExecuted{0};
    std::uint64_t remoteExecuted{0};
};

struct ExportFileRequest
{
    types::FileDescriptor source;
    QString outputPath;
    QString catalogPath;
    std::optional<types::DevelopParams> developParams;
    export_::RasterExportOptions options;
    bool useDefaultDevelopParamsWhenCatalogPhotoMissing{false};
};

struct ExportBatchRequest
{
    QString inputFolderPath;
    QString outputFolderPath;
    QString catalogPath;
    std::optional<types::DevelopParams> developParams;
    int workerCount{0};
    export_::RasterExportOptions options;
};

struct ExportItemListRequest
{
    QVector<ExportFileRequest> items;
};

using ExportRequest = std::variant<ExportFileRequest, ExportBatchRequest, ExportItemListRequest>;

struct ExportItemResult
{
    QString sourcePath;
    QString outputPath;
    bool succeeded{false};
    types::CoreError error;
    ExportItemFailureKind failureKind{ExportItemFailureKind::None};
};

struct ExportReport
{
    int totalCount{0};
    int succeededCount{0};
    int failedCount{0};
    QVector<ExportItemResult> items;
    ExportSchedulingMetrics scheduling;
};

struct ExportResult
{
    types::RequestId requestId{0};
    ExportReport report;
};

struct ExportProgress
{
    types::RequestId requestId{0};
    int completedCount{0};
    int totalCount{0};
    int succeededCount{0};
    int failedCount{0};
    QString currentSourcePath;
};

struct ExportIssue
{
    types::RequestId requestId{0};
    types::CoreError error;
};

using ExportSubmissionResult = types::Result<types::RequestId, types::CoreError>;

}  // namespace flexraw::core::orchestration

Q_DECLARE_METATYPE(flexraw::core::orchestration::ExportResult)
Q_DECLARE_METATYPE(flexraw::core::orchestration::ExportProgress)
Q_DECLARE_METATYPE(flexraw::core::orchestration::ExportIssue)

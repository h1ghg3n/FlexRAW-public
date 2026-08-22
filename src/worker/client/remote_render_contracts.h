#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include <QString>
#include <QUuid>
#include <QtTypes>

#include "render_contracts.h"
#include "result.h"

namespace flexraw::worker::client
{

struct RemoteRenderEndpoint
{
    QString host;
    quint16 port{0};
    std::chrono::milliseconds connectTimeout{5000};
    std::chrono::milliseconds renderTimeout{std::chrono::minutes(30)};
    std::chrono::milliseconds cancellationTimeout{5000};
};

struct RemoteWorkerProfile
{
    RemoteRenderEndpoint endpoint;
    QUuid expectedSourceStorageId;
    QUuid expectedOutputStorageId;
};

struct RemoteRenderRequest
{
    QString sourceRelativePath;
    QString outputRelativePath;
    core::types::DevelopParams developParams;
    core::export_::RasterExportOptions outputOptions;
};

enum class RemoteRenderErrorCode : std::uint8_t
{
    InvalidEndpoint,
    InvalidRequest,
    ConnectionFailed,
    ConnectionLost,
    TimedOut,
    ProtocolViolation,
    ServerBusy,
    ResourceBusy,
    RenderFailed,
    Cancelled,
};

struct RemoteRenderError
{
    RemoteRenderErrorCode code{RemoteRenderErrorCode::ConnectionFailed};
    core::types::CoreError cause;
    core::measurement::RenderStats stats;
    std::optional<std::chrono::milliseconds> retryAfter;
};

using RemoteRenderResult = core::types::Result<core::render::ResolvedRenderResult, RemoteRenderError>;

}  // namespace flexraw::worker::client

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include <QByteArray>
#include <QString>

#include "develop_params.h"
#include "error.h"
#include "export_options.h"
#include "render_stats.h"
#include "result.h"

namespace flexraw::worker::runtime
{

inline constexpr std::uint16_t RenderPayloadMajorVersion = 1;
inline constexpr std::uint16_t RenderPayloadMinorVersion = 0;
inline constexpr std::uint32_t MaximumWirePathBytes = 4096;
inline constexpr std::uint32_t MaximumWireMessageBytes = 64 * 1024;

struct RenderRequestPayload
{
    QString sourceRelativePath;
    QString outputRelativePath;
    core::types::DevelopParams developParams;
    core::export_::RasterExportOptions outputOptions;
};

struct RenderSucceededPayload
{
    QString outputRelativePath;
    std::uint64_t byteSize{0};
    core::measurement::RenderStats stats;
};

struct RenderFailedPayload
{
    core::types::CoreError cause;
    core::measurement::RenderStats stats;
};

struct ServerBusyPayload
{
    QString message;
};

struct ResourceBusyPayload
{
    QString message;
    std::optional<std::chrono::milliseconds> retryAfter;
};

enum class PayloadErrorCode
{
    UnsupportedVersion,
    MalformedPayload,
    TrailingBytes,
    InvalidUtf8,
    StringTooLong,
    InvalidEnumValue,
    InvalidDevelopParams,
    InvalidOutputOptions,
    PayloadTooLarge,
};

struct PayloadError
{
    PayloadErrorCode code{PayloadErrorCode::MalformedPayload};
    QString message;
};

using EncodePayloadResult = core::types::Result<QByteArray, PayloadError>;
using DecodeRenderRequestResult = core::types::Result<RenderRequestPayload, PayloadError>;
using DecodeRenderSucceededResult = core::types::Result<RenderSucceededPayload, PayloadError>;
using DecodeRenderFailedResult = core::types::Result<RenderFailedPayload, PayloadError>;
using DecodeServerBusyResult = core::types::Result<ServerBusyPayload, PayloadError>;
using DecodeResourceBusyResult = core::types::Result<ResourceBusyPayload, PayloadError>;

}  // namespace flexraw::worker::runtime

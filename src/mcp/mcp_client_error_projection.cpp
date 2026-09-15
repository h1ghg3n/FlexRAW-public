#include "mcp_client_error_projection.h"

#include <QString>

namespace flexraw::mcp
{
namespace
{

// 목적: ClientErrorCode를 stable MCP text code로 변환
// 입력: code: frontend-neutral common error category
// 출력: transport와 localization에 독립적인 snake_case code
[[nodiscard]] QString clientErrorCodeName(core::client::ClientErrorCode code)
{
    using core::client::ClientErrorCode;
    switch (code)
    {
    case ClientErrorCode::InvalidArgument:
        return QStringLiteral("invalid_argument");
    case ClientErrorCode::NotFound:
        return QStringLiteral("not_found");
    case ClientErrorCode::PermissionDenied:
        return QStringLiteral("permission_denied");
    case ClientErrorCode::UnsupportedFormat:
        return QStringLiteral("unsupported_format");
    case ClientErrorCode::ThumbnailUnavailable:
        return QStringLiteral("thumbnail_unavailable");
    case ClientErrorCode::DecodeFailed:
        return QStringLiteral("decode_failed");
    case ClientErrorCode::DatabaseError:
        return QStringLiteral("database_error");
    case ClientErrorCode::Conflict:
        return QStringLiteral("conflict");
    case ClientErrorCode::Cancelled:
        return QStringLiteral("cancelled");
    case ClientErrorCode::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

// 목적: ClientErrorCode를 model-visible한 비진단용 설명으로 변환
// 입력: code: frontend-neutral common error category
// 출력: technicalMessage를 노출하지 않는 안정된 English description
[[nodiscard]] QString clientErrorMessage(core::client::ClientErrorCode code)
{
    using core::client::ClientErrorCode;
    switch (code)
    {
    case ClientErrorCode::InvalidArgument:
        return QStringLiteral("The request arguments are invalid.");
    case ClientErrorCode::NotFound:
        return QStringLiteral("The requested resource was not found.");
    case ClientErrorCode::PermissionDenied:
        return QStringLiteral("The operation is not permitted.");
    case ClientErrorCode::UnsupportedFormat:
        return QStringLiteral("The requested format is unsupported.");
    case ClientErrorCode::ThumbnailUnavailable:
        return QStringLiteral("The requested thumbnail is unavailable.");
    case ClientErrorCode::DecodeFailed:
        return QStringLiteral("The requested content could not be decoded.");
    case ClientErrorCode::DatabaseError:
        return QStringLiteral("The persistent storage operation failed.");
    case ClientErrorCode::Conflict:
        return QStringLiteral("The current state conflicts with this request.");
    case ClientErrorCode::Cancelled:
        return QStringLiteral("The operation was cancelled.");
    case ClientErrorCode::Unknown:
        return QStringLiteral("The operation failed.");
    }
    return QStringLiteral("The operation failed.");
}

}  // namespace

// 목적: frontend-neutral ClientError를 diagnostic이 제거된 model-safe JSON으로 투영
// 입력: error: common error category와 diagnostic-only technical message
// 출력: stable code와 일반 설명만 포함한 error object
QJsonObject clientErrorToJson(const core::client::ClientError& error)
{
    return QJsonObject{{QStringLiteral("code"), clientErrorCodeName(error.code)},
                       {QStringLiteral("message"), clientErrorMessage(error.code)}};
}

// 목적: frontend-neutral ClientError를 model-safe MCP tool failure로 투영
// 입력: error: common error category와 diagnostic-only technical message
// 출력: stable code/message structured content와 분리된 diagnostic
McpToolCallResult makeClientErrorToolResult(const core::client::ClientError& error)
{
    return {.argumentsValid = true,
            .isError = true,
            .structuredContent = QJsonObject{{QStringLiteral("error"), clientErrorToJson(error)}},
            .diagnostic = error.technicalMessage};
}

}  // namespace flexraw::mcp

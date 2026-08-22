#pragma once

#include <QMetaType>
#include <QString>

namespace flexraw::core::types
{

enum class ErrorCode
{
    Unknown,
    InvalidArgument,
    NotFound,
    PermissionDenied,
    UnsupportedFormat,
    ThumbnailUnavailable,
    DecodeFailed,
    DatabaseError,
    Conflict,
    Cancelled,
};

struct CoreError
{
    ErrorCode code{ErrorCode::Unknown};
    QString message;
};

}  // namespace flexraw::core::types

Q_DECLARE_METATYPE(flexraw::core::types::ErrorCode)

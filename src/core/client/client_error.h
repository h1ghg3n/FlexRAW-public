#pragma once

#include <cstdint>
#include <string>

namespace flexraw::core::client
{

enum class ClientErrorCode : std::uint8_t
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

struct ClientError
{
    ClientErrorCode code{ClientErrorCode::Unknown};
    std::string technicalMessage;

    bool operator==(const ClientError&) const = default;
};

}  // namespace flexraw::core::client

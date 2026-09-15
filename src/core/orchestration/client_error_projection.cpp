#include "client_error_projection.h"

#include <cstddef>
#include <string>

#include <QByteArray>
#include <QString>

namespace flexraw::core::orchestration
{
namespace
{

// 목적: QString을 byte 길이가 보존된 UTF-8 client string으로 변환
// 입력: value: Qt 내부 문자열
// 출력: Qt-free UTF-8 string
[[nodiscard]] std::string toClientString(const QString& value)
{
    const QByteArray utf8 = value.toUtf8();
    return {utf8.constData(), static_cast<std::size_t>(utf8.size())};
}

// 목적: Qt-free UTF-8 client string을 QString으로 변환
// 입력: value: byte 길이를 가진 UTF-8 string
// 출력: transitional Qt 내부 문자열
[[nodiscard]] QString fromClientString(const std::string& value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

// 목적: Core error code를 Qt-free client error code로 변환
// 입력: code: domain 또는 Orchestration 오류 분류
// 출력: 같은 의미의 client enum
[[nodiscard]] client::ClientErrorCode toClientErrorCode(types::ErrorCode code) noexcept
{
    switch (code)
    {
    case types::ErrorCode::Unknown:
        return client::ClientErrorCode::Unknown;
    case types::ErrorCode::InvalidArgument:
        return client::ClientErrorCode::InvalidArgument;
    case types::ErrorCode::NotFound:
        return client::ClientErrorCode::NotFound;
    case types::ErrorCode::PermissionDenied:
        return client::ClientErrorCode::PermissionDenied;
    case types::ErrorCode::UnsupportedFormat:
        return client::ClientErrorCode::UnsupportedFormat;
    case types::ErrorCode::ThumbnailUnavailable:
        return client::ClientErrorCode::ThumbnailUnavailable;
    case types::ErrorCode::DecodeFailed:
        return client::ClientErrorCode::DecodeFailed;
    case types::ErrorCode::DatabaseError:
        return client::ClientErrorCode::DatabaseError;
    case types::ErrorCode::Conflict:
        return client::ClientErrorCode::Conflict;
    case types::ErrorCode::Cancelled:
        return client::ClientErrorCode::Cancelled;
    }
    return client::ClientErrorCode::Unknown;
}

// 목적: Qt-free client error code를 Core error code로 복원
// 입력: code: client command 오류 분류
// 출력: 같은 의미의 Core enum
[[nodiscard]] types::ErrorCode fromClientErrorCode(client::ClientErrorCode code) noexcept
{
    switch (code)
    {
    case client::ClientErrorCode::Unknown:
        return types::ErrorCode::Unknown;
    case client::ClientErrorCode::InvalidArgument:
        return types::ErrorCode::InvalidArgument;
    case client::ClientErrorCode::NotFound:
        return types::ErrorCode::NotFound;
    case client::ClientErrorCode::PermissionDenied:
        return types::ErrorCode::PermissionDenied;
    case client::ClientErrorCode::UnsupportedFormat:
        return types::ErrorCode::UnsupportedFormat;
    case client::ClientErrorCode::ThumbnailUnavailable:
        return types::ErrorCode::ThumbnailUnavailable;
    case client::ClientErrorCode::DecodeFailed:
        return types::ErrorCode::DecodeFailed;
    case client::ClientErrorCode::DatabaseError:
        return types::ErrorCode::DatabaseError;
    case client::ClientErrorCode::Conflict:
        return types::ErrorCode::Conflict;
    case client::ClientErrorCode::Cancelled:
        return types::ErrorCode::Cancelled;
    }
    return types::ErrorCode::Unknown;
}

}  // namespace

// 목적: Core 오류 분류와 진단문을 Qt-free client 오류로 투영
// 입력: error: Orchestration 또는 domain 오류
// 출력: 같은 분류와 UTF-8 technical message를 가진 client 오류
client::ClientError toClientError(const types::CoreError& error)
{
    return {toClientErrorCode(error.code), toClientString(error.message)};
}

// 목적: Qt-free client 오류를 transitional Qt 내부 오류로 복원
// 입력: error: client command가 반환한 오류
// 출력: 같은 분류와 QString technical message를 가진 Core 오류
types::CoreError fromClientError(const client::ClientError& error)
{
    return {fromClientErrorCode(error.code), fromClientString(error.technicalMessage)};
}

}  // namespace flexraw::core::orchestration

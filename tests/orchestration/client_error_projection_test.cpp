#include <array>
#include <utility>

#include <gtest/gtest.h>

#include "client_error_projection.h"

namespace flexraw::core::orchestration
{
namespace
{

TEST(ClientErrorProjectionTest, PreservesEveryKnownErrorCodeAndUtf8Diagnostics)
{
    constexpr std::array mappings{
        std::pair{types::ErrorCode::Unknown, client::ClientErrorCode::Unknown},
        std::pair{types::ErrorCode::InvalidArgument, client::ClientErrorCode::InvalidArgument},
        std::pair{types::ErrorCode::NotFound, client::ClientErrorCode::NotFound},
        std::pair{types::ErrorCode::PermissionDenied, client::ClientErrorCode::PermissionDenied},
        std::pair{types::ErrorCode::UnsupportedFormat, client::ClientErrorCode::UnsupportedFormat},
        std::pair{types::ErrorCode::ThumbnailUnavailable, client::ClientErrorCode::ThumbnailUnavailable},
        std::pair{types::ErrorCode::DecodeFailed, client::ClientErrorCode::DecodeFailed},
        std::pair{types::ErrorCode::DatabaseError, client::ClientErrorCode::DatabaseError},
        std::pair{types::ErrorCode::Conflict, client::ClientErrorCode::Conflict},
        std::pair{types::ErrorCode::Cancelled, client::ClientErrorCode::Cancelled},
    };

    for (const auto& [coreCode, clientCode] : mappings)
    {
        const types::CoreError original{coreCode, QStringLiteral("오류 진단")};
        const client::ClientError projected = toClientError(original);
        const types::CoreError restored = fromClientError(projected);

        EXPECT_EQ(clientCode, projected.code);
        EXPECT_EQ("오류 진단", projected.technicalMessage);
        EXPECT_EQ(coreCode, restored.code);
        EXPECT_EQ(original.message, restored.message);
    }
}

}  // namespace
}  // namespace flexraw::core::orchestration

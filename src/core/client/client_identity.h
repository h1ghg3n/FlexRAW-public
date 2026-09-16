#pragma once

#include <cstdint>

namespace flexraw::core::client
{

struct ClientPhotoId
{
    std::int64_t value{0};

    bool operator==(const ClientPhotoId&) const = default;
};

struct ClientProjectId
{
    std::int64_t value{0};

    bool operator==(const ClientProjectId&) const = default;
};

}  // namespace flexraw::core::client

#pragma once

#include <cstdint>

namespace flexraw::core::catalog
{

inline constexpr int MaximumCatalogCullingBatchSize = 200;

enum class CatalogCullingState : std::uint8_t
{
    Neutral = 0,
    Pick = 1,
    Reject = 2,
};

enum class CatalogCullingFilter : std::uint8_t
{
    All = 0,
    Pick = 1,
    Reject = 2,
};

}  // namespace flexraw::core::catalog

#pragma once

#include <cstdint>

namespace flexraw::core::catalog
{

inline constexpr int MaximumCatalogInclusionBatchSize = 200;

enum class CatalogInclusionState : std::uint8_t
{
    Included = 0,
    Excluded = 1,
};

enum class CatalogInclusionFilter : std::uint8_t
{
    IncludedOnly = 0,
    ExcludedOnly = 1,
};

}  // namespace flexraw::core::catalog

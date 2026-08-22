#pragma once

#include <cstdint>

namespace flexraw::core::measurement
{

struct RenderStats
{
    std::uint64_t decodeNanoseconds{0};
    std::uint64_t sourceConversionNanoseconds{0};
    std::uint64_t developNanoseconds{0};
    std::uint64_t outputNanoseconds{0};
    std::uint64_t totalNanoseconds{0};
};

struct SchedulerStats
{
    std::uint64_t queuedHighWater{0};
    std::uint64_t runningHighWater{0};
};

}  // namespace flexraw::core::measurement

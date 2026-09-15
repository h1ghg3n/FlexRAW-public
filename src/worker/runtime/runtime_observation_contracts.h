#pragma once

#include <cstdint>

#include "render_stats.h"

namespace flexraw::worker::runtime
{

struct WorkerRuntimeSnapshot
{
    std::uint64_t queued{0};
    std::uint64_t running{0};
    std::uint64_t maximumConcurrency{0};
    std::uint64_t queueCapacity{0};
    core::measurement::SchedulerStats highWater;
    bool accepting{false};
};

}  // namespace flexraw::worker::runtime

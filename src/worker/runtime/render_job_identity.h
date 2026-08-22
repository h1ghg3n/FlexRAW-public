#pragma once

#include <cstdint>

#include "job_id.h"

namespace flexraw::worker::runtime
{

using WorkerSessionId = std::uint64_t;

struct RenderJobKey
{
    WorkerSessionId sessionId{0};
    protocol::JobId jobId{0};

    // 목적: Worker 내부 active identity의 session/job 구성요소 비교
    // 입력: other: 비교할 composite key
    // 출력: sessionId와 JobId가 모두 같으면 true
    [[nodiscard]] bool operator==(const RenderJobKey& other) const = default;
};

}  // namespace flexraw::worker::runtime

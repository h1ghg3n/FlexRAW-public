#pragma once

#include <cstdint>

namespace flexraw::worker::runtime
{

using WorkerSessionId = std::uint64_t;

struct RenderJobId
{
    std::uint64_t value{0};

    // 목적: Runtime-owned render job identity의 고정 폭 값 비교
    // 입력: other: 비교할 Runtime job identity
    // 출력: 두 값이 같으면 true
    [[nodiscard]] bool operator==(const RenderJobId& other) const = default;
};

struct RenderJobKey
{
    WorkerSessionId sessionId{0};
    RenderJobId jobId;

    // 목적: Worker 내부 active identity의 session/job 구성요소 비교
    // 입력: other: 비교할 composite key
    // 출력: sessionId와 Runtime job identity가 모두 같으면 true
    [[nodiscard]] bool operator==(const RenderJobKey& other) const = default;
};

}  // namespace flexraw::worker::runtime

#pragma once

#include <cstdint>
#include <functional>

#include "render_job_identity.h"

namespace flexraw::worker::runtime
{

enum class RenderJobState : std::uint8_t
{
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelled,
};

struct RenderJobStatus
{
    RenderJobKey key;
    RenderJobState state{RenderJobState::Queued};
};

// callback은 submit/cancel/shutdown caller 또는 worker thread에서 직렬 호출될 수 있다. callback 안에서는 snapshot 같은
// read-only 조회만 허용하며 신속히 반환해야 한다. scheduler shutdown, owner 파괴 또는 callback lifetime 종료를 유발하면
// 안 된다.
using RenderJobStatusCallback = std::function<void(RenderJobStatus)>;

// 목적: render job state가 더 이상 전이하지 않는 terminal 상태인지 판정
// 입력: state: 판정할 lifecycle 상태
// 출력: Succeeded, Failed 또는 Cancelled이면 true
[[nodiscard]] constexpr bool isTerminalRenderJobState(const RenderJobState state) noexcept
{
    return state == RenderJobState::Succeeded || state == RenderJobState::Failed || state == RenderJobState::Cancelled;
}

}  // namespace flexraw::worker::runtime

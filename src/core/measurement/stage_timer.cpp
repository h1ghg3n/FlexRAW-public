#include "stage_timer.h"

namespace flexraw::core::measurement
{

// 목적: monotonic clock의 현재 시점을 stage 시작점으로 기록
// 입력: 없음
// 출력: 새 stage timer 객체
StageTimer::StageTimer() noexcept : m_startedAt(Clock::now()) {}

// 목적: 생성 이후 흐른 wall-clock 시간을 nanosecond 단위로 측정
// 입력: 없음
// 출력: 0 이상의 경과 nanosecond
std::uint64_t StageTimer::elapsedNanoseconds() const noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - m_startedAt).count();
    return static_cast<std::uint64_t>(elapsed);
}

}  // namespace flexraw::core::measurement

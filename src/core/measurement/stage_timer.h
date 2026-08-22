#pragma once

#include <chrono>
#include <cstdint>

namespace flexraw::core::measurement
{

class StageTimer final
{
public:
    // 목적: monotonic clock의 현재 시점을 stage 시작점으로 기록
    // 입력: 없음
    // 출력: 새 stage timer 객체
    StageTimer() noexcept;

    // 목적: 생성 이후 흐른 wall-clock 시간을 nanosecond 단위로 측정
    // 입력: 없음
    // 출력: 0 이상의 경과 nanosecond
    [[nodiscard]] std::uint64_t elapsedNanoseconds() const noexcept;

private:
    using Clock = std::chrono::steady_clock;

    Clock::time_point m_startedAt;
};

}  // namespace flexraw::core::measurement

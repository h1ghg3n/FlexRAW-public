#include "high_water_counter.h"

#include <cassert>

namespace flexraw::core::measurement
{

// 목적: 현재 count를 증가시키고 관찰된 최대값을 atomic하게 갱신
// 입력: 없음
// 출력: 증가 이후 current count
std::uint64_t HighWaterCounter::increment() noexcept
{
    const std::uint64_t current = m_current.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint64_t observedHighWater = m_highWater.load(std::memory_order_relaxed);
    while (observedHighWater < current &&
           !m_highWater.compare_exchange_weak(
               observedHighWater, current, std::memory_order_relaxed, std::memory_order_relaxed))
    {}
    return current;
}

// 목적: 현재 count를 underflow 없이 감소
// 입력: 없음
// 출력: 없음; current가 0인 호출은 programming error로 assert
void HighWaterCounter::decrement() noexcept
{
    std::uint64_t current = m_current.load(std::memory_order_relaxed);
    while (current > 0 &&
           !m_current.compare_exchange_weak(current, current - 1, std::memory_order_relaxed, std::memory_order_relaxed))
    {}
    assert(current > 0);
}

// 목적: 현재 count와 누적 high-water를 일관된 값 쌍으로 조회
// 입력: 없음
// 출력: 조회 시점의 current와 high-water snapshot
HighWaterSnapshot HighWaterCounter::snapshot() const noexcept
{
    for (;;)
    {
        const std::uint64_t current = m_current.load(std::memory_order_acquire);
        const std::uint64_t highWater = m_highWater.load(std::memory_order_acquire);
        if (highWater >= current)
        {
            return {current, highWater};
        }
    }
}

}  // namespace flexraw::core::measurement

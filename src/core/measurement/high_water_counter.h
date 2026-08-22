#pragma once

#include <atomic>
#include <cstdint>

namespace flexraw::core::measurement
{

struct HighWaterSnapshot
{
    std::uint64_t current{0};
    std::uint64_t highWater{0};
};

class HighWaterCounter final
{
public:
    // 목적: 비어 있는 concurrent current/high-water counter 생성
    // 입력: 없음
    // 출력: current와 high-water가 0인 counter
    HighWaterCounter() noexcept = default;

    HighWaterCounter(const HighWaterCounter&) = delete;
    HighWaterCounter& operator=(const HighWaterCounter&) = delete;

    // 목적: 현재 count를 증가시키고 관찰된 최대값을 atomic하게 갱신
    // 입력: 없음
    // 출력: 증가 이후 current count
    [[nodiscard]] std::uint64_t increment() noexcept;

    // 목적: 현재 count를 underflow 없이 감소
    // 입력: 없음
    // 출력: 없음; current가 0인 호출은 programming error로 assert
    void decrement() noexcept;

    // 목적: 현재 count와 누적 high-water를 일관된 값 쌍으로 조회
    // 입력: 없음
    // 출력: 조회 시점의 current와 high-water snapshot
    [[nodiscard]] HighWaterSnapshot snapshot() const noexcept;

private:
    std::atomic_uint64_t m_current{0};
    std::atomic_uint64_t m_highWater{0};
};

}  // namespace flexraw::core::measurement

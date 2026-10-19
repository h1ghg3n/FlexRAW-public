#pragma once

#include <memory>
#include <optional>

#include "operation_types.h"

namespace flexraw::core::orchestration
{

struct ComputeCapacitySnapshot
{
    int capacity{0};
    int permitsInUse{0};
    int permitHighWater{0};
    int waitingAcquisitions{0};
    bool closed{false};
};

struct ComputePermitState;

class ComputePermitPool;

class ComputePermitLease final
{
public:
    // 목적: 보유한 compute permit을 pool에 자동 반환
    // 입력: 없음
    // 출력: 없음
    ~ComputePermitLease();

    ComputePermitLease(const ComputePermitLease&) = delete;
    ComputePermitLease& operator=(const ComputePermitLease&) = delete;

    // 목적: 다른 lease의 compute permit 소유권을 이전
    // 입력: other — 소유권을 넘길 lease
    // 출력: 이동 생성된 lease
    ComputePermitLease(ComputePermitLease&& other) noexcept;

    // 목적: 기존 permit을 반환하고 다른 lease의 소유권을 이전
    // 입력: other — 소유권을 넘길 lease
    // 출력: 현재 lease 참조
    ComputePermitLease& operator=(ComputePermitLease&& other) noexcept;

    // 목적: 현재 lease가 보유한 compute permit 수 반환
    // 입력: 없음
    // 출력: 유효한 lease의 permit 수, 비어 있으면 0
    [[nodiscard]] int permitCount() const noexcept;

private:
    friend class ComputePermitPool;

    // 목적: pool이 승인한 compute permit 소유권을 lease로 구성
    // 입력: state — 공유 accounting 상태, permitCount — 승인된 permit 수
    // 출력: 새 lease
    ComputePermitLease(std::shared_ptr<ComputePermitState> state, int permitCount);

    // 목적: 보유 중인 compute permit을 중복 없이 반환
    // 입력: 없음
    // 출력: 없음
    void release() noexcept;

    std::shared_ptr<ComputePermitState> m_state;
    int m_permitCount{0};
};

class ComputePermitPool final
{
public:
    // 목적: 고정 capacity를 가진 process-local compute permit pool 생성
    // 입력: capacity — 동시에 대여할 수 있는 최대 permit 수
    // 출력: 없음
    explicit ComputePermitPool(int capacity);

    // 목적: 신규 acquire를 닫고 대기 중인 요청을 깨움
    // 입력: 없음
    // 출력: 없음
    ~ComputePermitPool();

    ComputePermitPool(const ComputePermitPool&) = delete;
    ComputePermitPool& operator=(const ComputePermitPool&) = delete;
    ComputePermitPool(ComputePermitPool&&) = delete;
    ComputePermitPool& operator=(ComputePermitPool&&) = delete;

    // 목적: cancellation과 pool 종료를 관찰하며 가용 범위 내 compute permit 대여
    // 입력: desiredPermits — 희망 수, minimumPermits — 최소 수, cancellationToken — 취소 상태
    // 출력: 승인된 move-only lease, 취소 또는 종료 시 nullopt
    [[nodiscard]] std::optional<ComputePermitLease> acquireUpTo(int desiredPermits,
                                                                int minimumPermits,
                                                                const types::CancellationToken& cancellationToken);

    // 목적: 현재 compute capacity accounting 상태를 일관된 snapshot으로 조회
    // 입력: 없음
    // 출력: capacity, 사용량, high-water, 대기자 수와 종료 상태
    [[nodiscard]] ComputeCapacitySnapshot snapshot() const;

    // 목적: 신규 acquire를 영구 차단하고 모든 대기 요청을 깨움
    // 입력: 없음
    // 출력: 없음
    void close();

private:
    std::shared_ptr<ComputePermitState> m_state;
};

}  // namespace flexraw::core::orchestration

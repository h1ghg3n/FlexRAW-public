#include "compute_permit_pool.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace flexraw::core::orchestration
{

namespace
{

constexpr auto CancellationPollInterval = std::chrono::milliseconds(5);

}  // namespace

struct ComputePermitState
{
    // 목적: immutable compute capacity와 비어 있는 runtime accounting 상태 초기화
    // 입력: initialCapacity — pool의 고정 permit 상한
    // 출력: 신규 process-local accounting 상태
    explicit ComputePermitState(int initialCapacity) : capacity(initialCapacity) {}

    std::mutex mutex;
    std::condition_variable available;
    int capacity{0};
    int permitsInUse{0};
    int permitHighWater{0};
    int waitingAcquisitions{0};
    bool closed{false};
};

ComputePermitLease::ComputePermitLease(std::shared_ptr<ComputePermitState> state, int permitCount)
    : m_state(std::move(state)), m_permitCount(permitCount)
{}

ComputePermitLease::~ComputePermitLease()
{
    release();
}

ComputePermitLease::ComputePermitLease(ComputePermitLease&& other) noexcept
    : m_state(std::move(other.m_state)), m_permitCount(std::exchange(other.m_permitCount, 0))
{}

ComputePermitLease& ComputePermitLease::operator=(ComputePermitLease&& other) noexcept
{
    if (this != &other)
    {
        release();
        m_state = std::move(other.m_state);
        m_permitCount = std::exchange(other.m_permitCount, 0);
    }
    return *this;
}

int ComputePermitLease::permitCount() const noexcept
{
    return m_permitCount;
}

void ComputePermitLease::release() noexcept
{
    if (m_state == nullptr || m_permitCount == 0)
    {
        return;
    }

    {
        const std::lock_guard lock(m_state->mutex);
        if (m_state->permitsInUse < m_permitCount)
        {
            std::terminate();
        }
        m_state->permitsInUse -= m_permitCount;
    }

    m_state->available.notify_all();
    m_state.reset();
    m_permitCount = 0;
}

ComputePermitPool::ComputePermitPool(int capacity)
{
    if (capacity <= 0)
    {
        throw std::invalid_argument("ComputePermitPool capacity must be positive.");
    }
    m_state = std::make_shared<ComputePermitState>(capacity);
}

ComputePermitPool::~ComputePermitPool()
{
    close();
}

std::optional<ComputePermitLease> ComputePermitPool::acquireUpTo(int desiredPermits,
                                                                 int minimumPermits,
                                                                 const types::CancellationToken& cancellationToken)
{
    if (desiredPermits <= 0 || minimumPermits <= 0 || minimumPermits > desiredPermits)
    {
        throw std::invalid_argument("Compute permit request bounds are invalid.");
    }

    const std::shared_ptr<ComputePermitState> state = m_state;
    if (minimumPermits > state->capacity)
    {
        throw std::invalid_argument("Minimum compute permit request exceeds pool capacity.");
    }

    std::unique_lock lock(state->mutex);
    ++state->waitingAcquisitions;

    try
    {
        while (true)
        {
            if (state->closed || cancellationToken.isCancellationRequested())
            {
                --state->waitingAcquisitions;
                return std::nullopt;
            }

            const int availablePermits = state->capacity - state->permitsInUse;
            if (availablePermits >= minimumPermits)
            {
                const int grantedPermits = std::min(desiredPermits, availablePermits);
                state->permitsInUse += grantedPermits;
                state->permitHighWater = std::max(state->permitHighWater, state->permitsInUse);
                --state->waitingAcquisitions;
                return ComputePermitLease(state, grantedPermits);
            }

            state->available.wait_for(lock, CancellationPollInterval);
        }
    }
    catch (...)
    {
        --state->waitingAcquisitions;
        throw;
    }
}

ComputeCapacitySnapshot ComputePermitPool::snapshot() const
{
    const std::lock_guard lock(m_state->mutex);
    return {
        .capacity = m_state->capacity,
        .permitsInUse = m_state->permitsInUse,
        .permitHighWater = m_state->permitHighWater,
        .waitingAcquisitions = m_state->waitingAcquisitions,
        .closed = m_state->closed,
    };
}

void ComputePermitPool::close()
{
    {
        const std::lock_guard lock(m_state->mutex);
        m_state->closed = true;
    }
    m_state->available.notify_all();
}

}  // namespace flexraw::core::orchestration

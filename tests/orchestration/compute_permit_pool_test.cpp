#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "compute_permit_pool.h"

namespace flexraw::core::orchestration
{
namespace
{

using namespace std::chrono_literals;

// 목적: 비동기 acquire가 실제 대기 상태에 도달할 때까지 bounded polling
// 입력: pool — 관찰할 permit pool
// 출력: 제한 시간 안에 대기자를 관찰했으면 true
[[nodiscard]] bool waitForAcquisition(const ComputePermitPool& pool)
{
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pool.snapshot().waitingAcquisitions > 0)
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

TEST(ComputePermitPoolTest, RejectsInvalidCapacityAndRequestBounds)
{
    EXPECT_THROW((void)ComputePermitPool(0), std::invalid_argument);

    ComputePermitPool pool(1);
    const types::CancellationSource cancellation;
    EXPECT_THROW((void)pool.acquireUpTo(0, 1, cancellation.token()), std::invalid_argument);
    EXPECT_THROW((void)pool.acquireUpTo(1, 0, cancellation.token()), std::invalid_argument);
    EXPECT_THROW((void)pool.acquireUpTo(1, 2, cancellation.token()), std::invalid_argument);
    EXPECT_THROW((void)pool.acquireUpTo(2, 2, cancellation.token()), std::invalid_argument);
}

TEST(ComputePermitPoolTest, GrantsAvailableCapacityAndReturnsItWithMovedLease)
{
    ComputePermitPool pool(3);
    const types::CancellationSource cancellation;

    {
        std::optional<ComputePermitLease> first = pool.acquireUpTo(2, 1, cancellation.token());
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ(2, first->permitCount());
        EXPECT_EQ(2, pool.snapshot().permitsInUse);

        ComputePermitLease moved = std::move(*first);
        EXPECT_EQ(2, moved.permitCount());
        EXPECT_EQ(0, first->permitCount());
    }

    const ComputeCapacitySnapshot snapshot = pool.snapshot();
    EXPECT_EQ(0, snapshot.permitsInUse);
    EXPECT_EQ(2, snapshot.permitHighWater);
}

TEST(ComputePermitPoolTest, WaitingAcquireContinuesAfterLeaseRelease)
{
    ComputePermitPool pool(1);
    const types::CancellationSource cancellation;
    std::optional<ComputePermitLease> held = pool.acquireUpTo(1, 1, cancellation.token());
    ASSERT_TRUE(held.has_value());
    std::promise<bool> completed;
    std::future<bool> result = completed.get_future();

    std::jthread waiter([&] {
        const std::optional<ComputePermitLease> acquired = pool.acquireUpTo(1, 1, cancellation.token());
        completed.set_value(acquired.has_value());
    });

    if (!waitForAcquisition(pool))
    {
        held.reset();
        waiter.join();
        FAIL() << "Acquire did not enter the waiting state.";
    }
    held.reset();
    const std::future_status status = result.wait_for(1s);
    if (status != std::future_status::ready)
    {
        pool.close();
        waiter.join();
        FAIL() << "Released permit did not wake the waiting acquire.";
    }
    EXPECT_TRUE(result.get());
}

TEST(ComputePermitPoolTest, CancellationStopsWaitingAcquireWithoutConsumingCapacity)
{
    ComputePermitPool pool(1);
    const types::CancellationSource holderCancellation;
    std::optional<ComputePermitLease> held = pool.acquireUpTo(1, 1, holderCancellation.token());
    ASSERT_TRUE(held.has_value());
    types::CancellationSource waitingCancellation;
    std::promise<bool> completed;
    std::future<bool> result = completed.get_future();

    std::jthread waiter([&] {
        const std::optional<ComputePermitLease> acquired = pool.acquireUpTo(1, 1, waitingCancellation.token());
        completed.set_value(acquired.has_value());
    });

    if (!waitForAcquisition(pool))
    {
        waitingCancellation.requestCancellation();
        held.reset();
        waiter.join();
        FAIL() << "Acquire did not enter the waiting state.";
    }
    waitingCancellation.requestCancellation();
    const std::future_status status = result.wait_for(1s);
    if (status != std::future_status::ready)
    {
        pool.close();
        waiter.join();
        FAIL() << "Cancellation did not wake the waiting acquire.";
    }
    EXPECT_FALSE(result.get());
    EXPECT_EQ(1, pool.snapshot().permitsInUse);
}

TEST(ComputePermitPoolTest, CloseWakesWaiterAndRejectsLaterAcquire)
{
    ComputePermitPool pool(1);
    const types::CancellationSource cancellation;
    std::optional<ComputePermitLease> held = pool.acquireUpTo(1, 1, cancellation.token());
    ASSERT_TRUE(held.has_value());
    std::promise<bool> completed;
    std::future<bool> result = completed.get_future();

    std::jthread waiter([&] {
        const std::optional<ComputePermitLease> acquired = pool.acquireUpTo(1, 1, cancellation.token());
        completed.set_value(acquired.has_value());
    });

    if (!waitForAcquisition(pool))
    {
        pool.close();
        waiter.join();
        FAIL() << "Acquire did not enter the waiting state.";
    }
    pool.close();
    ASSERT_EQ(std::future_status::ready, result.wait_for(1s));
    EXPECT_FALSE(result.get());
    EXPECT_FALSE(pool.acquireUpTo(1, 1, cancellation.token()).has_value());
    EXPECT_TRUE(pool.snapshot().closed);
}

}  // namespace
}  // namespace flexraw::core::orchestration

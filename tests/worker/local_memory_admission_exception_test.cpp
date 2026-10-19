#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>

#include <gtest/gtest.h>

#include "local_memory_admission.h"

namespace
{
thread_local int AllocationCountdown = -1;
// MSVC Debug의 noexcept string move가 사용하는 작은 iterator proxy는 실패 주입에서 제외합니다.
// 긴 request/client string과 map payload를 검증하며 모든 allocator bookkeeping 실패를 약속하지 않습니다.
constexpr std::size_t MinimumInjectedAllocationBytes = 64;
}  // namespace

// 목적: 이 독립 test executable의 acquire 호출 중 지정 allocation만 실패시킴
// 입력: size: allocation byte 수
// 출력: storage 또는 bad_alloc; 다른 thread와 비활성 구간은 정상 할당
void* operator new(std::size_t size)
{
    if (size >= MinimumInjectedAllocationBytes && AllocationCountdown == 0)
    {
        AllocationCountdown = -1;
        throw std::bad_alloc();
    }
    if (size >= MinimumInjectedAllocationBytes && AllocationCountdown > 0)
    {
        --AllocationCountdown;
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size))
    {
        return memory;
    }
    throw std::bad_alloc();
}

// 목적: test allocation을 같은 allocator로 반환
// 입력: memory: 반환할 storage
// 출력: 없음
void operator delete(void* memory) noexcept
{
    std::free(memory);
}

// 목적: sized delete 호출도 같은 allocator로 반환
// 입력: memory: 반환할 storage, size: 사용하지 않는 byte 수
// 출력: 없음
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace flexraw::worker::admission
{
namespace
{
class FixedMemoryProbe final : public platform::ISystemMemoryProbe
{
public:
    // 목적: allocation 없는 고정 memory snapshot 제공
    // 입력: 없음
    // 출력: reserve 제외 512 MiB headroom
    std::optional<platform::SystemMemorySnapshot> snapshot() const override
    {
        return platform::SystemMemorySnapshot{1024 * MebibyteBytes, 1024 * MebibyteBytes};
    }
};
}  // namespace

TEST(LocalMemoryAdmissionExceptionTest, FailedAcquireNeverCommitsAnUndeliveredLease)
{
    static_assert(std::is_nothrow_move_constructible_v<ResourceAdmissionDecision>);
    static_assert(std::is_nothrow_move_constructible_v<RenderResourceLease>);
    const ResourceLeaseRequest request{
        std::string(80, 'a'), std::string(80, 'c'), {400, 1.0, false}, std::chrono::seconds(60)};
    const ResourceLeaseRequest recovery{std::string(80, 'b'), "client", {400, 1.0, false}, std::chrono::seconds(60)};
    int failures = 0;
    bool reachedSuccess = false;
    for (int allocation = 0; allocation < 32; ++allocation)
    {
        FixedMemoryProbe probe;
        LocalMemoryAdmission admission(probe, 512);
        ResourceAdmissionDecision decision;
        bool threw = false;
        AllocationCountdown = allocation;
        try
        {
            decision = admission.acquire(request);
        }
        catch (const std::bad_alloc&)
        {
            threw = true;
        }
        AllocationCountdown = -1;
        SCOPED_TRACE(allocation);
        if (threw)
        {
            ++failures;
            const auto recovered = admission.acquire(recovery);
            EXPECT_TRUE(recovered.granted()) << "Failed acquire retained a commitment";
            if (recovered.granted())
            {
                EXPECT_TRUE(admission.release(*recovered.lease).released);
            }
        }
        else
        {
            ASSERT_TRUE(decision.granted());
            EXPECT_TRUE(admission.release(*decision.lease).released);
            reachedSuccess = true;
            break;
        }
    }
    EXPECT_GE(failures, 3);
    EXPECT_TRUE(reachedSuccess);
}
}  // namespace flexraw::worker::admission

#include "local_memory_admission.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace flexraw::worker::admission
{
namespace
{

// 목적: local admission result에 공통 Busy diagnostic 생성
// 입력: diagnostic: caller가 표시 또는 log에 사용할 bounded reason
// 출력: lease 없는 Busy decision
[[nodiscard]] ResourceAdmissionDecision makeBusyDecision(std::string diagnostic)
{
    return {ResourceAdmissionStatus::Busy, std::nullopt, std::nullopt, std::move(diagnostic)};
}

// 목적: local admission result에 공통 invalid request diagnostic 생성
// 입력: diagnostic: request contract 위반 reason
// 출력: lease 없는 InvalidRequest decision
[[nodiscard]] ResourceAdmissionDecision makeInvalidRequestDecision(std::string diagnostic)
{
    return {ResourceAdmissionStatus::InvalidRequest, std::nullopt, std::nullopt, std::move(diagnostic)};
}

// 목적: local admission result에 physical memory probe failure diagnostic 생성
// 입력: 없음
// 출력: lease 없는 Unavailable decision
[[nodiscard]] ResourceAdmissionDecision makeUnavailableDecision()
{
    return {ResourceAdmissionStatus::Unavailable,
            std::nullopt,
            std::nullopt,
            "Current host memory availability could not be determined."};
}

}  // namespace

// 목적: Platform memory snapshot과 minimum available reserve를 사용하는 local admission 생성
// 입력: memoryProbe: adapter보다 오래 사는 physical memory observer, reserveMiB: 유지할 최소 여유 memory
// 출력: active local lease commitment를 추적하는 admission implementation
LocalMemoryAdmission::LocalMemoryAdmission(const platform::ISystemMemoryProbe& memoryProbe,
                                           const std::uint64_t reserveMiB)
    : m_memoryProbe(memoryProbe), m_reserveMiB(reserveMiB)
{}

// 목적: local physical memory headroom과 active ledger를 함께 확인해 claim admission
// 입력: request: process-local lease identity와 예상 peak resource claim
// 출력: reserve를 유지할 수 있으면 local active lease, 아니면 Busy/Unavailable decision
ResourceAdmissionDecision LocalMemoryAdmission::acquire(const ResourceLeaseRequest& request)
{
    if (request.requestId.empty())
    {
        return makeInvalidRequestDecision("Local resource admission requires a non-empty request ID.");
    }
    if (!std::isfinite(request.claim.cpuCores) || request.claim.cpuCores < 0.0)
    {
        return makeInvalidRequestDecision("Local resource admission received an invalid CPU claim.");
    }

    const std::scoped_lock lock(m_mutex);
    const auto existingLease = m_activeLeases.find(request.requestId);
    if (existingLease != m_activeLeases.end())
    {
        const ActiveLeaseRecord& record = existingLease->second;
        if (record.clientId != request.clientId || record.claim.memoryMiB != request.claim.memoryMiB ||
            record.claim.cpuCores != request.claim.cpuCores || record.claim.requiresGpu != request.claim.requiresGpu ||
            record.ttl != request.ttl)
        {
            return makeInvalidRequestDecision("Local resource admission request ID was reused with a different claim.");
        }
        return {ResourceAdmissionStatus::Granted,
                RenderResourceLease{request.requestId, std::chrono::system_clock::time_point::max()},
                std::nullopt,
                {}};
    }

    const std::optional<platform::SystemMemorySnapshot> snapshot = m_memoryProbe.snapshot();
    if (!snapshot.has_value() || snapshot->availableBytes > snapshot->totalBytes ||
        snapshot->totalBytes / MebibyteBytes < m_reserveMiB || snapshot->availableBytes / MebibyteBytes < m_reserveMiB)
    {
        return snapshot.has_value() ? makeBusyDecision("Local minimum available memory reserve would be breached.")
                                    : makeUnavailableDecision();
    }

    const std::uint64_t totalMiB = snapshot->totalBytes / MebibyteBytes;
    const std::uint64_t availableMiB = snapshot->availableBytes / MebibyteBytes;
    const std::uint64_t allocatableMiB = totalMiB - m_reserveMiB;
    const std::uint64_t ledgerRemainingMiB =
        m_committedMemoryMiB >= allocatableMiB ? 0 : allocatableMiB - m_committedMemoryMiB;
    const std::uint64_t observedHeadroomMiB = availableMiB - m_reserveMiB;
    const std::uint64_t effectiveAvailableMiB = std::min(ledgerRemainingMiB, observedHeadroomMiB);
    if (request.claim.memoryMiB > effectiveAvailableMiB)
    {
        return makeBusyDecision("Local render memory claim would breach the configured available memory reserve.");
    }
    if (request.claim.memoryMiB > std::numeric_limits<std::uint64_t>::max() - m_committedMemoryMiB)
    {
        return makeInvalidRequestDecision("Local render memory claim overflows the active admission ledger.");
    }

    m_activeLeases.emplace(request.requestId, ActiveLeaseRecord{request.clientId, request.claim, request.ttl});
    m_committedMemoryMiB += request.claim.memoryMiB;
    return {ResourceAdmissionStatus::Granted,
            RenderResourceLease{request.requestId, std::chrono::system_clock::time_point::max()},
            std::nullopt,
            {}};
}

// 목적: local lease가 아직 active인지 확인하고 non-expiring lease 반환
// 입력: lease: local acquire가 발급한 opaque lease
// 출력: active면 Granted, 모르면 InvalidRequest
ResourceAdmissionDecision LocalMemoryAdmission::renew(const RenderResourceLease& lease, const std::chrono::seconds ttl)
{
    static_cast<void>(ttl);
    const std::scoped_lock lock(m_mutex);
    if (!m_activeLeases.contains(lease.id))
    {
        return makeInvalidRequestDecision("Local resource lease is no longer active.");
    }
    return {ResourceAdmissionStatus::Granted,
            RenderResourceLease{lease.id, std::chrono::system_clock::time_point::max()},
            std::nullopt,
            {}};
}

// 목적: local ledger에서 lease commitment를 idempotent하게 제거
// 입력: lease: local acquire가 발급한 opaque lease
// 출력: active 또는 이미 해제된 lease 모두 released true
ResourceLeaseReleaseResult LocalMemoryAdmission::release(const RenderResourceLease& lease) noexcept
{
    const std::scoped_lock lock(m_mutex);
    const auto iterator = m_activeLeases.find(lease.id);
    if (iterator == m_activeLeases.end())
    {
        return {true, {}};
    }
    m_committedMemoryMiB -= iterator->second.claim.memoryMiB;
    m_activeLeases.erase(iterator);
    return {true, {}};
}

}  // namespace flexraw::worker::admission

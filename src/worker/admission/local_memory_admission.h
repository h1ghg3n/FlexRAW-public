#pragma once

#include <mutex>
#include <unordered_map>

#include "render_resource_admission.h"
#include "system_memory_probe.h"

namespace flexraw::worker::admission
{

class LocalMemoryAdmission final : public IRenderResourceAdmission
{
public:
    // 목적: Platform memory snapshot과 minimum available reserve를 사용하는 local admission 생성
    // 입력: memoryProbe: adapter보다 오래 사는 physical memory observer, reserveMiB: 유지할 최소 여유 memory
    // 출력: active local lease commitment를 추적하는 admission implementation
    LocalMemoryAdmission(const platform::ISystemMemoryProbe& memoryProbe, std::uint64_t reserveMiB);

    // 목적: local physical memory headroom과 active ledger를 함께 확인해 claim admission
    // 입력: request: process-local lease identity와 예상 peak resource claim
    // 출력: reserve를 유지할 수 있으면 local active lease, 아니면 Busy/Unavailable decision
    [[nodiscard]] ResourceAdmissionDecision acquire(const ResourceLeaseRequest& request) override;

    // 목적: local lease가 아직 active인지 확인하고 non-expiring lease 반환
    // 입력: lease: local acquire가 발급한 opaque lease
    // 출력: active면 Granted, 모르면 InvalidRequest
    [[nodiscard]] ResourceAdmissionDecision renew(const RenderResourceLease& lease, std::chrono::seconds ttl) override;

    // 목적: local ledger에서 lease commitment를 idempotent하게 제거
    // 입력: lease: local acquire가 발급한 opaque lease
    // 출력: active 또는 이미 해제된 lease 모두 released true
    [[nodiscard]] ResourceLeaseReleaseResult release(const RenderResourceLease& lease) noexcept override;

private:
    struct ActiveLeaseRecord
    {
        std::string clientId;
        RenderResourceClaim claim;
        std::chrono::seconds ttl{0};
    };

    const platform::ISystemMemoryProbe& m_memoryProbe;
    const std::uint64_t m_reserveMiB;
    std::mutex m_mutex;
    std::unordered_map<std::string, ActiveLeaseRecord> m_activeLeases;
    std::uint64_t m_committedMemoryMiB{0};
};

}  // namespace flexraw::worker::admission

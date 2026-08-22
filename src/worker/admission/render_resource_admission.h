#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace flexraw::worker::admission
{

inline constexpr std::uint64_t MebibyteBytes = 1024ULL * 1024ULL;
inline constexpr std::uint64_t DefaultRenderMemoryClaimMiB = 640;

struct RenderResourceClaim
{
    std::uint64_t memoryMiB{0};
    double cpuCores{1.0};
    bool requiresGpu{false};
};

struct ResourceLeaseRequest
{
    std::string requestId;
    std::string clientId;
    RenderResourceClaim claim;
    std::chrono::seconds ttl{60};
};

struct RenderResourceLease
{
    std::string id;
    std::chrono::system_clock::time_point expiresAt{std::chrono::system_clock::time_point::max()};
};

enum class ResourceAdmissionStatus : std::uint8_t
{
    Granted,
    Busy,
    Unavailable,
    InvalidRequest,
};

struct ResourceAdmissionDecision
{
    ResourceAdmissionStatus status{ResourceAdmissionStatus::Unavailable};
    std::optional<RenderResourceLease> lease;
    std::optional<std::chrono::milliseconds> retryAfter;
    std::string diagnostic;

    // 목적: admission operation이 active lease를 성공적으로 반환했는지 판정
    // 입력: 없음
    // 출력: Granted 상태이고 lease가 있으면 true
    [[nodiscard]] bool granted() const noexcept
    {
        return status == ResourceAdmissionStatus::Granted && lease.has_value();
    }
};

struct ResourceLeaseReleaseResult
{
    bool released{false};
    std::string diagnostic;
};

struct ResourceAdmissionConfiguration
{
    std::uint64_t minimumAvailableMemoryMiB{512};
    std::uint64_t renderMemoryClaimMiB{DefaultRenderMemoryClaimMiB};
    double renderCpuCores{1.0};
    bool renderRequiresGpu{false};
    std::string resourceRouterUrl;
    std::string resourceRouterClientId{"flexraw-worker"};
    std::chrono::seconds resourceRouterLeaseTtl{60};
    std::chrono::milliseconds resourceRouterRequestTimeout{5000};
};

class IRenderResourceAdmission
{
public:
    virtual ~IRenderResourceAdmission() = default;

    // 목적: 새 render의 declared resource claim을 active lease로 admission
    // 입력: request: idempotency identity, client label, resource vector와 lease TTL
    // 출력: Granted lease 또는 Busy/Unavailable/InvalidRequest decision
    [[nodiscard]] virtual ResourceAdmissionDecision acquire(const ResourceLeaseRequest& request) = 0;

    // 목적: 실행 중 lease의 TTL을 연장해 declared resource commitment 유지
    // 입력: lease: acquire가 반환한 active lease, ttl: 갱신 요청 lease duration
    // 출력: 갱신된 Granted lease 또는 terminal/non-ready decision
    [[nodiscard]] virtual ResourceAdmissionDecision renew(const RenderResourceLease& lease,
                                                          std::chrono::seconds ttl) = 0;

    // 목적: associated render resource use 종료 후 lease commitment 해제
    // 입력: lease: 이전 acquire/renew가 반환한 active lease
    // 출력: Router 또는 local ledger가 release를 확인했으면 released true
    [[nodiscard]] virtual ResourceLeaseReleaseResult release(const RenderResourceLease& lease) noexcept = 0;
};

}  // namespace flexraw::worker::admission

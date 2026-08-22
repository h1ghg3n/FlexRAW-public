#pragma once

#include "render_resource_admission.h"

namespace flexraw::worker::admission
{

class ResourceRouterAdmission final : public IRenderResourceAdmission
{
public:
    // 목적: v1 Resource Router HTTP endpoint를 사용하는 render admission 생성
    // 입력: endpoint: Router base URL, timeout: 각 HTTP operation의 bounded wait
    // 출력: Qt Network 구현을 public contract 밖에 숨긴 lease adapter
    ResourceRouterAdmission(std::string endpoint, std::chrono::milliseconds timeout);

    // 목적: Router POST /v1/leases로 SHARED render claim을 acquire
    // 입력: request: v1 idempotency identity, client label, vector와 TTL
    // 출력: GRANTED lease 또는 Router BUSY/availability/validation decision
    [[nodiscard]] ResourceAdmissionDecision acquire(const ResourceLeaseRequest& request) override;

    // 목적: Router POST /v1/leases/{id}/renew로 active lease를 연장
    // 입력: lease: active Router lease, ttl: v1 renewal request duration
    // 출력: 갱신된 lease 또는 Router terminal/non-ready decision
    [[nodiscard]] ResourceAdmissionDecision renew(const RenderResourceLease& lease, std::chrono::seconds ttl) override;

    // 목적: Router DELETE /v1/leases/{id}로 lease를 idempotent release
    // 입력: lease: 이전 acquire/renew가 반환한 Router lease
    // 출력: 204 acknowledgment 여부와 transport diagnostic
    [[nodiscard]] ResourceLeaseReleaseResult release(const RenderResourceLease& lease) noexcept override;

private:
    std::string m_endpoint;
    std::chrono::milliseconds m_timeout;
};

}  // namespace flexraw::worker::admission

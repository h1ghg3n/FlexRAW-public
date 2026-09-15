#pragma once

#include "worker_health_probe_port.h"

namespace flexraw::worker::client
{

class WorkerHealthProbeAdapter final : public core::orchestration::IWorkerHealthProbePort
{
public:
    // 목적: Qt-free operation target을 existing Qt TCP health probe에 연결
    // 입력: target: UTF-8 endpoint와 bounded timeout
    // 출력: frontend-neutral reachability/compatibility/load 관측값
    [[nodiscard]] core::orchestration::WorkerHealthPortResult probe(
        const core::orchestration::WorkerHealthProbeTarget& target) const override;
};

}  // namespace flexraw::worker::client

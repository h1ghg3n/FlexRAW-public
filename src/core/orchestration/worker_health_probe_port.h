#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "client_error.h"
#include "client_result.h"
#include "worker_health_client.h"

namespace flexraw::core::orchestration
{

struct WorkerHealthProbeTarget
{
    std::string host;
    std::uint16_t port{0};
    std::chrono::milliseconds connectTimeout{2000};
    std::chrono::milliseconds responseTimeout{2000};
};

struct WorkerHealthProbeObservation
{
    client::WorkerReachability reachability{client::WorkerReachability::Unknown};
    client::WorkerCompatibility compatibility{client::WorkerCompatibility::Unknown};
    client::WorkerServiceState serviceState{client::WorkerServiceState::Unknown};
    std::optional<client::WorkerHealthLoadSnapshot> load;
    std::optional<std::int64_t> roundTripMilliseconds;
    std::optional<client::ClientError> issue;
};

using WorkerHealthPortResult = client::ClientResult<WorkerHealthProbeObservation, client::ClientError>;

class IWorkerHealthProbePort
{
public:
    // 목적: concrete Worker probe adapter를 interface pointer로 안전하게 소멸
    // 입력: 없음
    // 출력: 없음
    virtual ~IWorkerHealthProbePort() = default;

    // 목적: endpoint에 동기 one-shot probe를 실행하고 transport 관측값 반환
    // 입력: target: UTF-8 endpoint와 bounded connect/response timeout
    // 출력: health 관측값 또는 adapter 실행 오류
    [[nodiscard]] virtual WorkerHealthPortResult probe(const WorkerHealthProbeTarget& target) const = 0;
};

}  // namespace flexraw::core::orchestration

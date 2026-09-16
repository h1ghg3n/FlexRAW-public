#pragma once

#include <chrono>
#include <cstdint>

#include <QString>

#include "result.h"

namespace flexraw::worker::client
{

struct WorkerHealthEndpoint
{
    QString host;
    std::uint16_t port{0};
    std::chrono::milliseconds connectTimeout{2000};
    std::chrono::milliseconds responseTimeout{2000};
};

enum class WorkerHealthTransportServiceState
{
    Ready,
    Draining,
    ShuttingDown,
};

struct WorkerHealthTransportSnapshot
{
    WorkerHealthTransportServiceState serviceState{WorkerHealthTransportServiceState::Ready};
    std::uint64_t queuedJobs{0};
    std::uint64_t runningJobs{0};
    std::uint64_t maximumConcurrentJobs{0};
    std::uint64_t queueCapacity{0};
    std::chrono::milliseconds roundTrip{0};
};

enum class WorkerHealthTransportErrorCode
{
    InvalidEndpoint,
    ConnectionFailed,
    ConnectionLost,
    TimedOut,
    ProtocolViolation,
};

struct WorkerHealthTransportError
{
    WorkerHealthTransportErrorCode code{WorkerHealthTransportErrorCode::ConnectionFailed};
    QString message;
};

using WorkerHealthTransportResult = core::types::Result<WorkerHealthTransportSnapshot, WorkerHealthTransportError>;

class TcpWorkerHealthProbe final
{
public:
    // 목적: manual Worker endpoint에 동기 bounded health request 실행
    // 입력: endpoint: TCP 주소와 connect/response timeout
    // 출력: decoded runtime observation 또는 typed transport/protocol 오류
    [[nodiscard]] WorkerHealthTransportResult probe(const WorkerHealthEndpoint& endpoint) const;
};

}  // namespace flexraw::worker::client

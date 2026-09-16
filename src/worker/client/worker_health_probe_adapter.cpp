#include "worker_health_probe_adapter.h"

#include <optional>
#include <utility>

#include <QByteArray>
#include <QStringConverter>

#include "worker_health_probe.h"

namespace flexraw::worker::client
{
namespace
{

// 목적: UTF-8 Product Contract endpoint를 손실 없는 QString으로 변환
// 입력: value: profile에 저장된 UTF-8 byte sequence
// 출력: 유효한 UTF-8이면 QString, 아니면 nullopt
[[nodiscard]] std::optional<QString> decodeUtf8(const std::string& value)
{
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder.decode(QByteArray::fromStdString(value));
    return decoder.hasError() ? std::nullopt : std::optional<QString>{decoded};
}

// 목적: transport service state를 Product Contract enum으로 변환
// 입력: state: TCP health payload에서 검증된 상태
// 출력: 의미가 같은 frontend-neutral 상태
[[nodiscard]] core::client::WorkerServiceState toClientServiceState(
    const WorkerHealthTransportServiceState state) noexcept
{
    switch (state)
    {
    case WorkerHealthTransportServiceState::Ready:
        return core::client::WorkerServiceState::Ready;
    case WorkerHealthTransportServiceState::Draining:
        return core::client::WorkerServiceState::Draining;
    case WorkerHealthTransportServiceState::ShuttingDown:
        return core::client::WorkerServiceState::ShuttingDown;
    }
    return core::client::WorkerServiceState::Unknown;
}

// 목적: transport failure를 사용자 문구와 분리된 structured health observation으로 변환
// 입력: error: connection/protocol/timeout 분류와 technical message
// 출력: reachability와 compatibility 의미가 보존된 관측값
[[nodiscard]] core::orchestration::WorkerHealthProbeObservation toFailureObservation(
    const WorkerHealthTransportError& error)
{
    core::orchestration::WorkerHealthProbeObservation observation;
    observation.issue =
        core::client::ClientError{core::client::ClientErrorCode::Unknown, error.message.toUtf8().toStdString()};
    switch (error.code)
    {
    case WorkerHealthTransportErrorCode::ConnectionFailed:
        observation.reachability = core::client::WorkerReachability::Unreachable;
        break;
    case WorkerHealthTransportErrorCode::ProtocolViolation:
        observation.reachability = core::client::WorkerReachability::Reachable;
        observation.compatibility = core::client::WorkerCompatibility::Incompatible;
        break;
    case WorkerHealthTransportErrorCode::ConnectionLost:
    case WorkerHealthTransportErrorCode::TimedOut:
        observation.reachability = core::client::WorkerReachability::Reachable;
        break;
    case WorkerHealthTransportErrorCode::InvalidEndpoint:
        break;
    }
    return observation;
}

}  // namespace

// 목적: Qt-free operation target을 existing Qt TCP health probe에 연결
// 입력: target: UTF-8 endpoint와 bounded timeout
// 출력: frontend-neutral reachability/compatibility/load 관측값
core::orchestration::WorkerHealthPortResult WorkerHealthProbeAdapter::probe(
    const core::orchestration::WorkerHealthProbeTarget& target) const
{
    const std::optional<QString> host = decodeUtf8(target.host);
    if (!host.has_value())
    {
        return core::orchestration::WorkerHealthPortResult::failure(
            {core::client::ClientErrorCode::InvalidArgument, "Worker health endpoint is not valid UTF-8."});
    }

    const TcpWorkerHealthProbe probe;
    const WorkerHealthTransportResult result =
        probe.probe({*host, target.port, target.connectTimeout, target.responseTimeout});
    if (result.hasError())
    {
        if (result.error().code == WorkerHealthTransportErrorCode::InvalidEndpoint)
        {
            return core::orchestration::WorkerHealthPortResult::failure(
                {core::client::ClientErrorCode::InvalidArgument, result.error().message.toUtf8().toStdString()});
        }
        return core::orchestration::WorkerHealthPortResult::success(toFailureObservation(result.error()));
    }

    const WorkerHealthTransportSnapshot& snapshot = result.value();
    core::orchestration::WorkerHealthProbeObservation observation;
    observation.reachability = core::client::WorkerReachability::Reachable;
    observation.compatibility = core::client::WorkerCompatibility::Compatible;
    observation.serviceState = toClientServiceState(snapshot.serviceState);
    observation.load = core::client::WorkerHealthLoadSnapshot{
        snapshot.runningJobs, snapshot.queuedJobs, snapshot.maximumConcurrentJobs, snapshot.queueCapacity};
    observation.roundTripMilliseconds = snapshot.roundTrip.count();
    return core::orchestration::WorkerHealthPortResult::success(std::move(observation));
}

}  // namespace flexraw::worker::client

#include "worker_health_orchestrator.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

#include <QMetaObject>
#include <QThread>

namespace flexraw::core::orchestration
{
namespace
{

// 목적: 현재 system clock을 Qt-free Unix millisecond timestamp로 변환
// 입력: 없음
// 출력: Unix epoch 이후 millisecond
[[nodiscard]] std::int64_t currentUnixMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 목적: probe worker thread의 예외를 frontend-neutral diagnostic 오류로 변환
// 입력: message: exception 상세 또는 fallback 설명
// 출력: Unknown 분류의 ClientError
[[nodiscard]] client::ClientError makeUnhandledProbeError(std::string message)
{
    return {client::ClientErrorCode::Unknown, std::move(message)};
}

}  // namespace

// 목적: application-scoped Worker profile lookup과 bounded one-shot probe lifecycle 조립
// 입력: profileClient: endpoint source, probePort: 동기 transport adapter, parent: Qt lifetime owner
// 출력: 한 번에 하나의 probe와 profile별 최근 snapshot을 소유하는 Orchestrator
WorkerHealthOrchestrator::WorkerHealthOrchestrator(client::IWorkerProfileClient& profileClient,
                                                   std::unique_ptr<IWorkerHealthProbePort> probePort,
                                                   QObject* const parent)
    : QObject(parent), m_profileClient(&profileClient), m_probePort(std::move(probePort))
{
    if (m_probePort == nullptr)
    {
        throw std::invalid_argument("Worker health probe port must not be null.");
    }
    m_probePool.setMaxThreadCount(1);
}

// 목적: 신규 probe를 차단하고 in-flight transport call 종료를 기다림
// 입력: 없음
// 출력: 이후 frontend event를 만들지 않는 종료 상태
WorkerHealthOrchestrator::~WorkerHealthOrchestrator()
{
    m_acceptingProbes = false;
    m_probePool.waitForDone();
    m_activeProbe.reset();
}

// 목적: 저장된 profile endpoint에 bounded one-shot health probe 제출
// 입력: profileId: endpoint를 해석할 stable Worker profile identity
// 출력: accepted probe receipt 또는 validation·profile·busy 오류
client::WorkerHealthProbeResult WorkerHealthOrchestrator::probeWorker(const client::WorkerProfileId& profileId)
{
    if (QThread::currentThread() != thread())
    {
        return client::WorkerHealthProbeResult::failure(
            {client::ClientErrorCode::Conflict, "Worker health commands must run on the owner thread."});
    }
    if (!m_acceptingProbes)
    {
        return client::WorkerHealthProbeResult::failure(
            {client::ClientErrorCode::Conflict, "Worker health service is shutting down."});
    }
    const client::WorkerProfileResult profileResult = findProfile(profileId);
    if (profileResult.hasError())
    {
        return client::WorkerHealthProbeResult::failure(profileResult.error());
    }
    if (m_activeProbe.has_value())
    {
        return client::WorkerHealthProbeResult::failure(
            {client::ClientErrorCode::Conflict, "Another Worker health probe is already active."});
    }

    const client::WorkerProfileSnapshot& profile = profileResult.value();
    if (m_nextProbeId == 0)
    {
        m_nextProbeId = 1;
    }
    const client::WorkerHealthProbeReceipt receipt{{m_nextProbeId++}, profile.id};
    const WorkerHealthProbeTarget target{profile.host, profile.port};
    m_activeProbe = ActiveProbe{receipt, profile.host, profile.port};
    emit workerHealthProbeAccepted(receipt);

    const IWorkerHealthProbePort* const probePort = m_probePort.get();
    m_probePool.start([this, probePort, receipt, target]() mutable {
        WorkerHealthPortResult result = [&]() {
            try
            {
                return probePort->probe(target);
            }
            catch (const std::exception& error)
            {
                return WorkerHealthPortResult::failure(
                    makeUnhandledProbeError(std::string("Unhandled Worker health probe exception: ") + error.what()));
            }
            catch (...)
            {
                return WorkerHealthPortResult::failure(
                    makeUnhandledProbeError("Unhandled non-standard Worker health probe exception."));
            }
        }();
        static_cast<void>(QMetaObject::invokeMethod(
            this,
            [this, receipt, target, result = std::move(result)]() mutable {
                finishProbe(receipt, target, std::move(result));
            },
            Qt::QueuedConnection));
    });
    return client::WorkerHealthProbeResult::success(receipt);
}

// 목적: 현재 profile endpoint와 일치하는 가장 최근 health 관측값 조회
// 입력: profileId: 조회할 stable Worker profile identity
// 출력: 미관측 Unknown 또는 최근 snapshot, profile 조회 오류
client::WorkerHealthSnapshotResult WorkerHealthOrchestrator::workerHealthSnapshot(
    const client::WorkerProfileId& profileId)
{
    if (QThread::currentThread() != thread())
    {
        return client::WorkerHealthSnapshotResult::failure(
            {client::ClientErrorCode::Conflict, "Worker health state must be read on the owner thread."});
    }
    const client::WorkerProfileResult profileResult = findProfile(profileId);
    if (profileResult.hasError())
    {
        return client::WorkerHealthSnapshotResult::failure(profileResult.error());
    }

    const client::WorkerProfileSnapshot& profile = profileResult.value();
    const auto cached = m_cachedSnapshots.find(profile.id.value);
    if (cached == m_cachedSnapshots.cend() || cached->second.host != profile.host ||
        cached->second.port != profile.port)
    {
        return client::WorkerHealthSnapshotResult::success(makeUnknownSnapshot(profile.id));
    }
    return client::WorkerHealthSnapshotResult::success(cached->second.snapshot);
}

// 목적: event adapter initial state에 사용할 현재 active probe 조회
// 입력: 없음
// 출력: in-flight probe가 있으면 receipt, 없으면 nullopt
std::optional<client::WorkerHealthProbeReceipt> WorkerHealthOrchestrator::activeProbe() const
{
    return m_activeProbe.has_value() ? std::optional<client::WorkerHealthProbeReceipt>{m_activeProbe->receipt}
                                     : std::nullopt;
}

// 목적: stable identity에 해당하는 최신 Worker profile snapshot 조회
// 입력: profileId: 찾을 profile identity
// 출력: profile 또는 저장소·not-found 오류
client::WorkerProfileResult WorkerHealthOrchestrator::findProfile(const client::WorkerProfileId& profileId)
{
    if (profileId.value.empty())
    {
        return client::WorkerProfileResult::failure(
            {client::ClientErrorCode::InvalidArgument, "Worker profile identity must not be empty."});
    }
    const client::WorkerProfileListResult profiles = m_profileClient->listWorkerProfiles();
    if (profiles.hasError())
    {
        return client::WorkerProfileResult::failure(profiles.error());
    }
    const auto profile = std::ranges::find(profiles.value(), profileId, &client::WorkerProfileSnapshot::id);
    if (profile == profiles.value().cend())
    {
        return client::WorkerProfileResult::failure(
            {client::ClientErrorCode::NotFound, "Worker profile was not found."});
    }
    return client::WorkerProfileResult::success(*profile);
}

// 목적: transport 결과를 현재 profile에만 적용하고 accepted probe terminal 발행
// 입력: receipt/target: accepted context, result: worker thread의 probe 결과
// 출력: cache 갱신과 exact completion signal
void WorkerHealthOrchestrator::finishProbe(client::WorkerHealthProbeReceipt receipt,
                                           WorkerHealthProbeTarget target,
                                           WorkerHealthPortResult result)
{
    if (!m_acceptingProbes || !m_activeProbe.has_value() || m_activeProbe->receipt != receipt)
    {
        return;
    }

    const std::int64_t observedAt = currentUnixMilliseconds();
    client::WorkerHealthSnapshot snapshot = makeUnknownSnapshot(receipt.profileId);
    const client::WorkerProfileResult currentProfile = findProfile(receipt.profileId);
    const bool targetStillCurrent = currentProfile.hasValue() && currentProfile.value().host == target.host &&
                                    currentProfile.value().port == target.port;
    if (!targetStillCurrent)
    {
        snapshot.issue = currentProfile.hasError()
                             ? std::optional<client::ClientError>{currentProfile.error()}
                             : std::optional<client::ClientError>{client::ClientError{
                                   client::ClientErrorCode::Conflict,
                                   "Worker profile endpoint changed while its health probe was active."}};
        snapshot.observedAtUnixMilliseconds = observedAt;
    }
    else
    {
        const auto previous = m_cachedSnapshots.find(receipt.profileId.value);
        if (previous != m_cachedSnapshots.cend() && previous->second.host == target.host &&
            previous->second.port == target.port)
        {
            snapshot.lastSeenAtUnixMilliseconds = previous->second.snapshot.lastSeenAtUnixMilliseconds;
        }

        if (result.hasError())
        {
            snapshot.issue = result.error();
        }
        else
        {
            const WorkerHealthProbeObservation& observation = result.value();
            snapshot.reachability = observation.reachability;
            snapshot.compatibility = observation.compatibility;
            snapshot.serviceState = observation.serviceState;
            snapshot.load = observation.load;
            snapshot.roundTripMilliseconds = observation.roundTripMilliseconds;
            snapshot.issue = observation.issue;
            if (observation.reachability == client::WorkerReachability::Reachable &&
                observation.compatibility == client::WorkerCompatibility::Compatible)
            {
                snapshot.lastSeenAtUnixMilliseconds = observedAt;
            }
        }
        snapshot.observedAtUnixMilliseconds = observedAt;
        m_cachedSnapshots.insert_or_assign(receipt.profileId.value, CachedSnapshot{target.host, target.port, snapshot});
    }

    m_activeProbe.reset();
    emit workerHealthProbeCompleted({std::move(receipt), std::move(snapshot)});
}

// 목적: profile에 아직 유효한 관측이 없음을 나타내는 snapshot 생성
// 입력: profileId: snapshot identity, issue: optional 구조화된 원인
// 출력: 모든 health dimension이 Unknown인 snapshot
client::WorkerHealthSnapshot WorkerHealthOrchestrator::makeUnknownSnapshot(const client::WorkerProfileId& profileId,
                                                                           std::optional<client::ClientError> issue)
{
    client::WorkerHealthSnapshot snapshot;
    snapshot.profileId = profileId;
    snapshot.issue = std::move(issue);
    return snapshot;
}

}  // namespace flexraw::core::orchestration

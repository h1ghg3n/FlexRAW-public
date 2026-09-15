#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <QObject>
#include <QThreadPool>

#include "worker_health_client.h"
#include "worker_health_probe_port.h"

namespace flexraw::core::orchestration
{

class WorkerHealthOrchestrator final : public QObject, public client::IWorkerHealthClient
{
    Q_OBJECT

public:
    // 목적: application-scoped Worker profile lookup과 bounded one-shot probe lifecycle 조립
    // 입력: profileClient: endpoint source, probePort: 동기 transport adapter, parent: Qt lifetime owner
    // 출력: 한 번에 하나의 probe와 profile별 최근 snapshot을 소유하는 Orchestrator
    WorkerHealthOrchestrator(client::IWorkerProfileClient& profileClient,
                             std::unique_ptr<IWorkerHealthProbePort> probePort,
                             QObject* parent = nullptr);

    // 목적: 신규 probe를 차단하고 in-flight transport call 종료를 기다림
    // 입력: 없음
    // 출력: 이후 frontend event를 만들지 않는 종료 상태
    ~WorkerHealthOrchestrator() override;

    // 목적: 저장된 profile endpoint에 bounded one-shot health probe 제출
    // 입력: profileId: endpoint를 해석할 stable Worker profile identity
    // 출력: accepted probe receipt 또는 validation·profile·busy 오류
    [[nodiscard]] client::WorkerHealthProbeResult probeWorker(const client::WorkerProfileId& profileId) override;

    // 목적: 현재 profile endpoint와 일치하는 가장 최근 health 관측값 조회
    // 입력: profileId: 조회할 stable Worker profile identity
    // 출력: 미관측 Unknown 또는 최근 snapshot, profile 조회 오류
    [[nodiscard]] client::WorkerHealthSnapshotResult workerHealthSnapshot(
        const client::WorkerProfileId& profileId) override;

    // 목적: event adapter initial state에 사용할 현재 active probe 조회
    // 입력: 없음
    // 출력: in-flight probe가 있으면 receipt, 없으면 nullopt
    [[nodiscard]] std::optional<client::WorkerHealthProbeReceipt> activeProbe() const;

signals:
    // 목적: accepted probe identity를 adapter가 비동기 Product event로 투영하도록 알림
    // 입력: receipt: owner가 발급한 probe와 profile identity
    // 출력: 없음
    void workerHealthProbeAccepted(const client::WorkerHealthProbeReceipt& receipt);

    // 목적: accepted probe의 terminal observation을 adapter에 전달
    // 입력: completion: probe identity와 immutable health snapshot
    // 출력: 없음
    void workerHealthProbeCompleted(const client::WorkerHealthProbeCompletion& completion);

private:
    struct ActiveProbe
    {
        client::WorkerHealthProbeReceipt receipt;
        std::string host;
        std::uint16_t port{0};
    };

    struct CachedSnapshot
    {
        std::string host;
        std::uint16_t port{0};
        client::WorkerHealthSnapshot snapshot;
    };

    // 목적: stable identity에 해당하는 최신 Worker profile snapshot 조회
    // 입력: profileId: 찾을 profile identity
    // 출력: profile 또는 저장소·not-found 오류
    [[nodiscard]] client::WorkerProfileResult findProfile(const client::WorkerProfileId& profileId);

    // 목적: transport 결과를 현재 profile에만 적용하고 accepted probe terminal 발행
    // 입력: receipt/target: accepted context, result: worker thread의 probe 결과
    // 출력: cache 갱신과 exact completion signal
    void finishProbe(client::WorkerHealthProbeReceipt receipt,
                     WorkerHealthProbeTarget target,
                     WorkerHealthPortResult result);

    // 목적: profile에 아직 유효한 관측이 없음을 나타내는 snapshot 생성
    // 입력: profileId: snapshot identity, issue: optional 구조화된 원인
    // 출력: 모든 health dimension이 Unknown인 snapshot
    [[nodiscard]] static client::WorkerHealthSnapshot makeUnknownSnapshot(
        const client::WorkerProfileId& profileId, std::optional<client::ClientError> issue = std::nullopt);

    client::IWorkerProfileClient* m_profileClient{nullptr};
    std::unique_ptr<IWorkerHealthProbePort> m_probePort;
    QThreadPool m_probePool;
    std::optional<ActiveProbe> m_activeProbe;
    std::unordered_map<std::string, CachedSnapshot> m_cachedSnapshots;
    std::uint64_t m_nextProbeId{1};
    bool m_acceptingProbes{true};
};

}  // namespace flexraw::core::orchestration

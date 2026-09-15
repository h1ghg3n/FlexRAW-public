#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include "client_error.h"
#include "client_result.h"
#include "worker_profile_client.h"

namespace flexraw::core::client
{

struct WorkerHealthProbeId
{
    std::uint64_t value{0};

    bool operator==(const WorkerHealthProbeId&) const = default;
};

struct WorkerHealthProbeReceipt
{
    WorkerHealthProbeId id;
    WorkerProfileId profileId;

    bool operator==(const WorkerHealthProbeReceipt&) const = default;
};

enum class WorkerReachability : std::uint8_t
{
    Unknown,
    Reachable,
    Unreachable,
};

enum class WorkerCompatibility : std::uint8_t
{
    Unknown,
    Compatible,
    Incompatible,
};

enum class WorkerServiceState : std::uint8_t
{
    Unknown,
    Ready,
    Draining,
    ShuttingDown,
};

struct WorkerHealthLoadSnapshot
{
    std::uint64_t runningJobs{0};
    std::uint64_t queuedJobs{0};
    std::uint64_t maximumConcurrentJobs{0};
    std::uint64_t queueCapacity{0};

    bool operator==(const WorkerHealthLoadSnapshot&) const = default;
};

struct WorkerHealthSnapshot
{
    WorkerProfileId profileId;
    WorkerReachability reachability{WorkerReachability::Unknown};
    WorkerCompatibility compatibility{WorkerCompatibility::Unknown};
    WorkerServiceState serviceState{WorkerServiceState::Unknown};
    std::optional<WorkerHealthLoadSnapshot> load;
    std::optional<std::int64_t> roundTripMilliseconds;
    std::optional<std::int64_t> observedAtUnixMilliseconds;
    std::optional<std::int64_t> lastSeenAtUnixMilliseconds;
    std::optional<ClientError> issue;

    bool operator==(const WorkerHealthSnapshot&) const = default;
};

struct WorkerHealthProbeCompletion
{
    WorkerHealthProbeReceipt receipt;
    WorkerHealthSnapshot snapshot;

    bool operator==(const WorkerHealthProbeCompletion&) const = default;
};

struct WorkerHealthEventSequence
{
    std::uint64_t value{0};

    bool operator==(const WorkerHealthEventSequence&) const = default;
};

struct WorkerHealthEvent
{
    WorkerHealthEventSequence sequence;
    bool initial{false};
    std::optional<WorkerHealthProbeReceipt> activeProbe;
    std::optional<WorkerHealthProbeCompletion> completion;

    bool operator==(const WorkerHealthEvent&) const = default;
};

using WorkerHealthProbeResult = ClientResult<WorkerHealthProbeReceipt, ClientError>;
using WorkerHealthSnapshotResult = ClientResult<WorkerHealthSnapshot, ClientError>;
using WorkerHealthCallback = std::function<void(const WorkerHealthEvent&)>;

class IWorkerHealthClient
{
public:
    // 목적: implementation별 Worker health command/state resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IWorkerHealthClient() = default;

    // 목적: 저장된 profile endpoint에 bounded one-shot health probe 제출
    // 입력: profileId: endpoint를 해석할 stable Worker profile identity
    // 출력: accepted probe receipt 또는 validation·profile·busy 오류
    [[nodiscard]] virtual WorkerHealthProbeResult probeWorker(const WorkerProfileId& profileId) = 0;

    // 목적: profile endpoint에 대응하는 가장 최근 health 관측값 조회
    // 입력: profileId: 조회할 stable Worker profile identity
    // 출력: 미관측 Unknown 또는 최근 snapshot, profile 조회 오류
    [[nodiscard]] virtual WorkerHealthSnapshotResult workerHealthSnapshot(const WorkerProfileId& profileId) = 0;
};

class IWorkerHealthSubscription
{
public:
    // 목적: implementation별 Worker health subscription resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IWorkerHealthSubscription() = default;

    // 목적: queued event와 이후 Worker health callback 전달 차단
    // 입력: 없음
    // 출력: 없음; 여러 번 호출해도 같은 inactive 상태 유지
    virtual void unsubscribe() noexcept = 0;

    // 목적: subscription이 이후 callback을 받을 수 있는지 조회
    // 입력: 없음
    // 출력: callback 전달이 허용된 상태이면 true
    [[nodiscard]] virtual bool isActive() const noexcept = 0;
};

using WorkerHealthSubscriptionHandle = std::shared_ptr<IWorkerHealthSubscription>;
using WorkerHealthSubscriptionResult = ClientResult<WorkerHealthSubscriptionHandle, ClientError>;

class IWorkerHealthEventSource
{
public:
    // 목적: implementation별 Worker health event resource를 concrete destructor로 정리
    // 입력: 없음
    // 출력: 없음
    virtual ~IWorkerHealthEventSource() = default;

    // 목적: adapter delivery context에서 active probe와 이후 terminal snapshot 구독
    // 입력: callback: immutable Worker health event consumer
    // 출력: unsubscribe lifetime handle 또는 callback·delivery context 오류
    [[nodiscard]] virtual WorkerHealthSubscriptionResult subscribeToWorkerHealth(WorkerHealthCallback callback) = 0;
};

}  // namespace flexraw::core::client

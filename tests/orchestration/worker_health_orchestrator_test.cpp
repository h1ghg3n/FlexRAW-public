#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSemaphore>
#include <QThread>

#include <gtest/gtest.h>

#include "test_application.h"
#include "worker_health_orchestrator.h"

namespace flexraw::core::orchestration
{
namespace
{

using namespace std::chrono_literals;

// 목적: Qt event를 처리하며 asynchronous health condition 충족 대기
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 predicate가 참이면 true
[[nodiscard]] bool waitUntil(const std::function<bool()>& predicate, const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

class WorkerProfileClientFake final : public client::IWorkerProfileClient
{
public:
    // 목적: health Orchestrator test가 조회할 in-memory profile 목록 구성
    // 입력: profiles: stable identity와 endpoint snapshot 목록
    // 출력: mutation을 지원하지 않는 fake client
    explicit WorkerProfileClientFake(std::vector<client::WorkerProfileSnapshot> profiles)
        : m_profiles(std::move(profiles))
    {}

    // 목적: test Orchestrator가 해석할 deterministic profile 목록 반환
    // 입력: 없음
    // 출력: 현재 in-memory profile 목록
    [[nodiscard]] client::WorkerProfileListResult listWorkerProfiles() override
    {
        return client::WorkerProfileListResult::success(m_profiles);
    }

    // 목적: 이 read-only fake에서 지원하지 않는 create command 거절
    // 입력: command: 사용하지 않는 profile 값
    // 출력: Conflict 오류
    [[nodiscard]] client::WorkerProfileResult createWorkerProfile(
        const client::CreateWorkerProfileCommand& command) override
    {
        static_cast<void>(command);
        return unsupported();
    }

    // 목적: 이 read-only fake에서 지원하지 않는 update command 거절
    // 입력: command: 사용하지 않는 profile 값
    // 출력: Conflict 오류
    [[nodiscard]] client::WorkerProfileResult updateWorkerProfile(
        const client::UpdateWorkerProfileCommand& command) override
    {
        static_cast<void>(command);
        return unsupported();
    }

    // 목적: 이 read-only fake에서 지원하지 않는 remove command 거절
    // 입력: id: 사용하지 않는 profile identity
    // 출력: Conflict 오류
    [[nodiscard]] client::WorkerProfileRemoveResult removeWorkerProfile(const client::WorkerProfileId& id) override
    {
        static_cast<void>(id);
        return client::WorkerProfileRemoveResult::failure(
            {client::ClientErrorCode::Conflict, "Profile mutation is unavailable in this test fake."});
    }

    // 목적: 이 read-only fake에서 지원하지 않는 enable command 거절
    // 입력: command: 사용하지 않는 policy 값
    // 출력: Conflict 오류
    [[nodiscard]] client::WorkerProfileResult setWorkerProfileEnabled(
        const client::SetWorkerProfileEnabledCommand& command) override
    {
        static_cast<void>(command);
        return unsupported();
    }

private:
    // 목적: 반복되는 unsupported profile mutation result 생성
    // 입력: 없음
    // 출력: Conflict WorkerProfileResult
    [[nodiscard]] static client::WorkerProfileResult unsupported()
    {
        return client::WorkerProfileResult::failure(
            {client::ClientErrorCode::Conflict, "Profile mutation is unavailable in this test fake."});
    }

    std::vector<client::WorkerProfileSnapshot> m_profiles;
};

class SequencedWorkerHealthProbe final : public IWorkerHealthProbePort
{
public:
    // 목적: invocation별 반환할 deterministic probe result queue 구성
    // 입력: results: worker thread에서 순서대로 꺼낼 결과
    // 출력: thread-safe fake probe port
    explicit SequencedWorkerHealthProbe(std::deque<WorkerHealthPortResult> results) : m_results(std::move(results)) {}

    // 목적: probe invocation마다 준비된 observation을 순서대로 반환
    // 입력: target: 호출 여부만 검증하는 endpoint
    // 출력: queue의 다음 deterministic result
    [[nodiscard]] WorkerHealthPortResult probe(const WorkerHealthProbeTarget& target) const override
    {
        const std::scoped_lock lock(m_mutex);
        EXPECT_EQ("127.0.0.1", target.host);
        EXPECT_EQ(47331, target.port);
        if (m_results.empty())
        {
            return WorkerHealthPortResult::failure(
                {client::ClientErrorCode::Unknown, "No test health result remains."});
        }
        WorkerHealthPortResult result = std::move(m_results.front());
        m_results.pop_front();
        return result;
    }

private:
    mutable std::mutex m_mutex;
    mutable std::deque<WorkerHealthPortResult> m_results;
};

class BlockingWorkerHealthProbe final : public IWorkerHealthProbePort
{
public:
    // 목적: concurrent submission test가 release할 때까지 probe thread block
    // 입력: target: 사용하지 않는 valid endpoint
    // 출력: release 뒤 reachable compatible snapshot
    [[nodiscard]] WorkerHealthPortResult probe(const WorkerHealthProbeTarget& target) const override
    {
        static_cast<void>(target);
        m_started.release();
        m_release.acquire();
        return WorkerHealthPortResult::success({client::WorkerReachability::Reachable,
                                                client::WorkerCompatibility::Compatible,
                                                client::WorkerServiceState::Ready,
                                                client::WorkerHealthLoadSnapshot{0, 0, 1, 2},
                                                1,
                                                std::nullopt});
    }

    // 목적: background probe가 blocking section에 진입할 때까지 대기
    // 입력: 없음
    // 출력: 1초 안에 시작했으면 true
    [[nodiscard]] bool waitForStarted() const
    {
        return m_started.tryAcquire(1, 1000);
    }

    // 목적: blocking probe를 정상 observation 경로로 진행
    // 입력: 없음
    // 출력: worker semaphore release
    void release() const
    {
        m_release.release();
    }

private:
    mutable QSemaphore m_started;
    mutable QSemaphore m_release;
};

// 목적: health Orchestrator test용 stable Worker profile 생성
// 입력: 없음
// 출력: loopback endpoint를 가진 enabled profile
[[nodiscard]] client::WorkerProfileSnapshot makeProfile()
{
    return {{"worker-1"}, "Local Worker", "127.0.0.1", 47331, true, {}, {}};
}

TEST(WorkerHealthOrchestratorTest, PublishesCompatibleSnapshotWithConfiguredLoad)
{
    static_cast<void>(test::application());
    WorkerProfileClientFake profiles({makeProfile()});
    auto probe = std::make_unique<SequencedWorkerHealthProbe>(std::deque<WorkerHealthPortResult>{
        WorkerHealthPortResult::success({client::WorkerReachability::Reachable,
                                         client::WorkerCompatibility::Compatible,
                                         client::WorkerServiceState::Ready,
                                         client::WorkerHealthLoadSnapshot{1, 2, 4, 8},
                                         12,
                                         std::nullopt})});
    WorkerHealthOrchestrator orchestrator(profiles, std::move(probe));
    std::optional<client::WorkerHealthProbeCompletion> completion;
    QObject::connect(&orchestrator,
                     &WorkerHealthOrchestrator::workerHealthProbeCompleted,
                     &orchestrator,
                     [&completion](const client::WorkerHealthProbeCompletion& value) { completion = value; });

    const client::WorkerHealthProbeResult accepted = orchestrator.probeWorker(makeProfile().id);

    ASSERT_TRUE(accepted.hasValue());
    ASSERT_TRUE(waitUntil([&completion]() { return completion.has_value(); }));
    EXPECT_EQ(accepted.value(), completion->receipt);
    EXPECT_EQ(client::WorkerReachability::Reachable, completion->snapshot.reachability);
    EXPECT_EQ(client::WorkerCompatibility::Compatible, completion->snapshot.compatibility);
    ASSERT_TRUE(completion->snapshot.load.has_value());
    EXPECT_EQ(client::WorkerHealthLoadSnapshot({1, 2, 4, 8}), *completion->snapshot.load);
    EXPECT_EQ(12, completion->snapshot.roundTripMilliseconds);
    EXPECT_TRUE(completion->snapshot.observedAtUnixMilliseconds.has_value());
    EXPECT_EQ(completion->snapshot.observedAtUnixMilliseconds, completion->snapshot.lastSeenAtUnixMilliseconds);

    const client::WorkerHealthSnapshotResult cached = orchestrator.workerHealthSnapshot(makeProfile().id);
    ASSERT_TRUE(cached.hasValue());
    EXPECT_EQ(completion->snapshot, cached.value());
}

TEST(WorkerHealthOrchestratorTest, PreservesLastSeenWhenLaterProbeIsUnreachable)
{
    static_cast<void>(test::application());
    WorkerProfileClientFake profiles({makeProfile()});
    auto probe = std::make_unique<SequencedWorkerHealthProbe>(std::deque<WorkerHealthPortResult>{
        WorkerHealthPortResult::success({client::WorkerReachability::Reachable,
                                         client::WorkerCompatibility::Compatible,
                                         client::WorkerServiceState::Ready,
                                         client::WorkerHealthLoadSnapshot{0, 0, 2, 3},
                                         4,
                                         std::nullopt}),
        WorkerHealthPortResult::success(
            {client::WorkerReachability::Unreachable,
             client::WorkerCompatibility::Unknown,
             client::WorkerServiceState::Unknown,
             std::nullopt,
             std::nullopt,
             client::ClientError{client::ClientErrorCode::Unknown, "connection refused"}})});
    WorkerHealthOrchestrator orchestrator(profiles, std::move(probe));
    std::vector<client::WorkerHealthProbeCompletion> completions;
    QObject::connect(
        &orchestrator,
        &WorkerHealthOrchestrator::workerHealthProbeCompleted,
        &orchestrator,
        [&completions](const client::WorkerHealthProbeCompletion& value) { completions.push_back(value); });

    ASSERT_TRUE(orchestrator.probeWorker(makeProfile().id).hasValue());
    ASSERT_TRUE(waitUntil([&completions]() { return completions.size() == 1; }));
    const std::optional<std::int64_t> firstLastSeen = completions.front().snapshot.lastSeenAtUnixMilliseconds;
    ASSERT_TRUE(firstLastSeen.has_value());
    ASSERT_TRUE(orchestrator.probeWorker(makeProfile().id).hasValue());
    ASSERT_TRUE(waitUntil([&completions]() { return completions.size() == 2; }));

    EXPECT_EQ(client::WorkerReachability::Unreachable, completions.back().snapshot.reachability);
    EXPECT_EQ(firstLastSeen, completions.back().snapshot.lastSeenAtUnixMilliseconds);
    EXPECT_TRUE(completions.back().snapshot.issue.has_value());
}

TEST(WorkerHealthOrchestratorTest, RejectsConcurrentProbeWithoutChangingSchedulingPolicy)
{
    static_cast<void>(test::application());
    WorkerProfileClientFake profiles({makeProfile()});
    auto probe = std::make_unique<BlockingWorkerHealthProbe>();
    BlockingWorkerHealthProbe* const probePointer = probe.get();
    WorkerHealthOrchestrator orchestrator(profiles, std::move(probe));

    const client::WorkerHealthProbeResult first = orchestrator.probeWorker(makeProfile().id);
    ASSERT_TRUE(first.hasValue());
    ASSERT_TRUE(probePointer->waitForStarted());
    const client::WorkerHealthProbeResult second = orchestrator.probeWorker(makeProfile().id);
    ASSERT_TRUE(second.hasError());
    EXPECT_EQ(client::ClientErrorCode::Conflict, second.error().code);

    probePointer->release();
    ASSERT_TRUE(waitUntil([&orchestrator]() { return !orchestrator.activeProbe().has_value(); }));
}

TEST(WorkerHealthOrchestratorTest, DestructionWaitsForInFlightProbeAndDropsQueuedCompletion)
{
    static_cast<void>(test::application());
    WorkerProfileClientFake profiles({makeProfile()});
    auto probe = std::make_unique<BlockingWorkerHealthProbe>();
    BlockingWorkerHealthProbe* const probePointer = probe.get();
    auto orchestrator = std::make_unique<WorkerHealthOrchestrator>(profiles, std::move(probe));
    int completionCount = 0;
    QObject::connect(orchestrator.get(),
                     &WorkerHealthOrchestrator::workerHealthProbeCompleted,
                     orchestrator.get(),
                     [&completionCount](const client::WorkerHealthProbeCompletion&) { ++completionCount; });
    ASSERT_TRUE(orchestrator->probeWorker(makeProfile().id).hasValue());
    ASSERT_TRUE(probePointer->waitForStarted());
    std::thread releaseThread([probePointer]() {
        std::this_thread::sleep_for(20ms);
        probePointer->release();
    });

    orchestrator.reset();
    releaseThread.join();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    EXPECT_EQ(0, completionCount);
}

}  // namespace
}  // namespace flexraw::core::orchestration

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QSemaphore>
#include <QThread>

#include <gtest/gtest.h>

#include "export_orchestrator.h"
#include "system_memory_probe.h"
#include "test_application.h"

namespace flexraw::core::orchestration
{
namespace
{

using namespace std::chrono_literals;

// 목적: 비동기 scheduler callback을 처리하면서 deterministic 조건 대기
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: 제한 시간 안에 조건이 충족되면 true
[[nodiscard]] bool waitForCondition(const std::function<bool()>& predicate, const int timeoutMilliseconds = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMilliseconds)
    {
        if (predicate())
        {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

// 목적: fake pipeline 검증용 batch request 생성
// 입력: workerCount: request-local Local concurrency cap
// 출력: submit validation을 통과하는 batch request
[[nodiscard]] ExportRequest makeBatchRequest(const int workerCount = 0)
{
    ExportBatchRequest request;
    request.inputFolderPath = QStringLiteral("input");
    request.outputFolderPath = QStringLiteral("output");
    request.workerCount = workerCount;
    return request;
}

// 목적: placement policy와 optional valid Remote target 조립
// 입력: policy: 실행 정책, includeTarget: manual target 포함 여부
// 출력: test용 placement option
[[nodiscard]] ExportPlacementOptions makePlacement(const ExportPlacementPolicy policy, const bool includeTarget = true)
{
    ExportPlacementOptions placement;
    placement.policy = policy;
    if (includeTarget)
    {
        placement.remoteTarget = ExportRemoteTarget{
            QStringLiteral("worker.test"), 47331, QStringLiteral("source-storage"), QStringLiteral("output-storage")};
    }
    return placement;
}

// 목적: 짧은 test cooldown과 명시적 slot limit를 가진 scheduler 설정 생성
// 입력: localSlots/remoteSlots: target별 hard ceiling
// 출력: resource reserve가 비활성화된 deterministic 설정
[[nodiscard]] ExportSchedulingConfiguration makeConfiguration(const int localSlots = 2, const int remoteSlots = 2)
{
    ExportSchedulingConfiguration configuration;
    configuration.localSlotLimit = localSlots;
    configuration.remoteSlotLimit = remoteSlots;
    configuration.maximumRemoteDispatchAttempts = 3;
    configuration.enforceLocalResourceReserve = false;
    configuration.serverBusyCooldown = 5ms;
    configuration.resourceBusyCooldown = 5ms;
    configuration.connectionFailedCooldown = 5ms;
    return configuration;
}

struct ExportObservation
{
    std::optional<ExportResult> completed;
    std::optional<ExportIssue> failed;
    std::vector<types::RequestId> cancelled;
};

// 목적: scheduler terminal signal을 test-owned snapshot에 연결
// 입력: orchestrator: 관찰 대상, observation: 결과 저장소
// 출력: object lifetime 동안 유지되는 direct/queued signal 연결
void observe(ExportOrchestrator& orchestrator, ExportObservation& observation)
{
    QObject::connect(&orchestrator, &ExportOrchestrator::exportCompleted, [&](const ExportResult& result) {
        observation.completed = result;
    });
    QObject::connect(&orchestrator, &ExportOrchestrator::exportFailed, [&](const ExportIssue& issue) {
        observation.failed = issue;
    });
    QObject::connect(&orchestrator, &ExportOrchestrator::exportCancelled, [&](const types::RequestId requestId) {
        observation.cancelled.push_back(requestId);
    });
}

enum class LocalBehavior : std::uint8_t
{
    Succeed,
    BlockUntilReleased,
    BlockUntilCancelled,
};

class ScriptedExportPipeline final : public IExportPipeline
{
public:
    // 목적: item 수와 Local execution 동작이 고정된 fake pipeline 생성
    // 입력: itemCount: prepare 결과 수, behavior: item execution blocking 정책
    // 출력: deterministic pipeline 객체
    explicit ScriptedExportPipeline(const int itemCount, const LocalBehavior behavior = LocalBehavior::Succeed)
        : m_itemCount(itemCount), m_behavior(behavior)
    {}

    // 목적: filesystem 접근 없이 immutable item corpus 생성
    // 입력: request: 사용하지 않음, cancellationToken: preparation 취소 확인
    // 출력: 지정된 수의 unique source/output item
    [[nodiscard]] ExportPreparationResult prepare(const ExportRequest&,
                                                  const types::CancellationToken& cancellationToken) const override
    {
        if (cancellationToken.isCancellationRequested())
        {
            return ExportPreparationResult::failure(
                {types::ErrorCode::Cancelled, QStringLiteral("Fake preparation cancelled.")});
        }

        PreparedExport prepared;
        prepared.items.reserve(m_itemCount);
        for (int index = 0; index < m_itemCount; ++index)
        {
            const QString sourcePath = QStringLiteral("item-%1.arw").arg(index);
            const types::FileDescriptor source{
                sourcePath, QStringLiteral("arw"), sourcePath, types::SupportedFileKind::Raw};
            prepared.items.push_back(
                {ExportFileRequest{source, QStringLiteral("item-%1.jpg").arg(index), {}, types::DevelopParams{}, {}},
                 {}});
        }
        return ExportPreparationResult::success(std::move(prepared));
    }

    // 목적: Local slot 사용과 시작 순서를 기록하고 configured blocking 적용
    // 입력: item: 실행 item, cancellationToken: cooperative cancellation 상태
    // 출력: 성공 또는 cancellation item 결과
    [[nodiscard]] ExportItemResult executeItem(const PreparedExportItem& item,
                                               const types::CancellationToken& cancellationToken) const override
    {
        const int active = m_active.fetch_add(1, std::memory_order_relaxed) + 1;
        int previousMaximum = m_maxActive.load(std::memory_order_relaxed);
        while (active > previousMaximum &&
               !m_maxActive.compare_exchange_weak(previousMaximum, active, std::memory_order_relaxed))
        {}
        m_callCount.fetch_add(1, std::memory_order_relaxed);
        {
            const std::scoped_lock lock(m_orderMutex);
            m_startOrder.push_back(item.request.source.path);
        }

        if (m_behavior == LocalBehavior::BlockUntilReleased)
        {
            while (!cancellationToken.isCancellationRequested() && !m_release.tryAcquire(1, 2))
            {}
        }
        else if (m_behavior == LocalBehavior::BlockUntilCancelled)
        {
            while (!cancellationToken.isCancellationRequested())
            {
                QThread::msleep(1);
            }
        }

        m_active.fetch_sub(1, std::memory_order_relaxed);
        if (cancellationToken.isCancellationRequested())
        {
            return {item.request.source.path,
                    item.request.outputPath,
                    false,
                    {types::ErrorCode::Cancelled, QStringLiteral("Fake Local execution cancelled.")},
                    ExportItemFailureKind::Execution};
        }
        return {item.request.source.path, item.request.outputPath, true, {}, ExportItemFailureKind::None};
    }

    // 목적: item scheduler test에서 사용되지 않는 legacy entry point 명시
    // 입력: request/token/progress: 사용하지 않음
    // 출력: 호출되면 test failure로 식별 가능한 오류
    [[nodiscard]] ExportPipelineResult execute(const ExportRequest&,
                                               const types::CancellationToken&,
                                               const ExportProgressCallback&) const override
    {
        return ExportPipelineResult::failure(
            {types::ErrorCode::Unknown, QStringLiteral("Legacy execute must not be used by placement tests.")});
    }

    // 목적: blocked Local item을 지정 수만큼 진행 허용
    // 입력: count: release할 item 수
    // 출력: semaphore token 추가
    void release(const int count = 1) const
    {
        m_release.release(count);
    }

    // 목적: 현재까지 시작된 Local item 수 조회
    // 입력: 없음
    // 출력: thread-safe call counter
    [[nodiscard]] int callCount() const
    {
        return m_callCount.load(std::memory_order_relaxed);
    }

    // 목적: 관찰된 Local 동시 실행 high-water 조회
    // 입력: 없음
    // 출력: maximum active item 수
    [[nodiscard]] int maximumActive() const
    {
        return m_maxActive.load(std::memory_order_relaxed);
    }

    // 목적: Local item 시작 순서 snapshot 반환
    // 입력: 없음
    // 출력: source path 순서
    [[nodiscard]] std::vector<QString> startOrder() const
    {
        const std::scoped_lock lock(m_orderMutex);
        return m_startOrder;
    }

private:
    int m_itemCount{0};
    LocalBehavior m_behavior{LocalBehavior::Succeed};
    mutable QSemaphore m_release;
    mutable std::atomic_int m_callCount{0};
    mutable std::atomic_int m_active{0};
    mutable std::atomic_int m_maxActive{0};
    mutable std::mutex m_orderMutex;
    mutable std::vector<QString> m_startOrder;
};

enum class RemoteOutcome : std::uint8_t
{
    Success,
    Ineligible,
    ServerBusy,
    ResourceBusy,
    ConnectionFailed,
    ConnectionLost,
    TimedOut,
    ProtocolViolation,
    RenderFailed,
};

struct RemoteStep
{
    RemoteOutcome outcome{RemoteOutcome::Success};
    bool accepted{true};
    bool blockBeforeAccepted{false};
    bool blockAfterAccepted{false};
    std::optional<std::chrono::milliseconds> retryAfter;
};

class ScriptedRemotePort final : public IRemoteExportExecutionPort
{
public:
    // 목적: Remote dispatch별 고정 결과 sequence를 가진 fake port 생성
    // 입력: steps: call 순서별 acceptance/blocking/outcome
    // 출력: deterministic Remote execution adapter
    explicit ScriptedRemotePort(std::vector<RemoteStep> steps) : m_steps(std::move(steps)) {}

    // 목적: handshake와 running lifecycle을 기록해 normalized Remote 결과 반환
    // 입력: target/item/token/accepted: Remote execution port 계약
    // 출력: scripted success 또는 typed failure
    [[nodiscard]] RemoteExportExecutionResult execute(const ExportRemoteTarget&,
                                                      const PreparedExportItem& item,
                                                      const types::CancellationToken& cancellationToken,
                                                      const RemoteExportAcceptedCallback& accepted) const override
    {
        const int callIndex = m_callCount.fetch_add(1, std::memory_order_relaxed);
        const RemoteStep step = callIndex < static_cast<int>(m_steps.size()) ? m_steps[callIndex] : RemoteStep{};
        const int activeHandshake = m_activeHandshakes.fetch_add(1, std::memory_order_relaxed) + 1;
        updateMaximum(m_maxHandshakes, activeHandshake);

        if (step.blockBeforeAccepted)
        {
            m_dispatchingCount.fetch_add(1, std::memory_order_relaxed);
            while (!cancellationToken.isCancellationRequested() && !m_handshakeRelease.tryAcquire(1, 2))
            {}
        }
        if (cancellationToken.isCancellationRequested())
        {
            m_activeHandshakes.fetch_sub(1, std::memory_order_relaxed);
            m_finishedCount.fetch_add(1, std::memory_order_relaxed);
            return cancelledFailure(false);
        }

        if (step.accepted)
        {
            accepted();
            m_acceptedCount.fetch_add(1, std::memory_order_relaxed);
        }
        m_activeHandshakes.fetch_sub(1, std::memory_order_relaxed);

        if (step.accepted)
        {
            const int activeExecution = m_activeExecutions.fetch_add(1, std::memory_order_relaxed) + 1;
            updateMaximum(m_maxExecutions, activeExecution);
            if (step.blockAfterAccepted)
            {
                m_runningCount.fetch_add(1, std::memory_order_relaxed);
                while (!cancellationToken.isCancellationRequested() && !m_executionRelease.tryAcquire(1, 2))
                {}
            }
            m_activeExecutions.fetch_sub(1, std::memory_order_relaxed);
        }

        m_finishedCount.fetch_add(1, std::memory_order_relaxed);
        if (cancellationToken.isCancellationRequested())
        {
            return cancelledFailure(step.accepted);
        }
        if (step.outcome == RemoteOutcome::Success)
        {
            return RemoteExportExecutionResult::success(
                {item.request.source.path, item.request.outputPath, true, {}, ExportItemFailureKind::None});
        }
        return RemoteExportExecutionResult::failure(makeFailure(step));
    }

    // 목적: blocked Remote handshake를 지정 수만큼 진행 허용
    // 입력: count: release할 handshake 수
    // 출력: semaphore token 추가
    void releaseHandshake(const int count = 1) const
    {
        m_handshakeRelease.release(count);
    }

    // 목적: accepted 후 blocked Remote execution을 지정 수만큼 진행 허용
    // 입력: count: release할 running job 수
    // 출력: semaphore token 추가
    void releaseExecution(const int count = 1) const
    {
        m_executionRelease.release(count);
    }

    // 목적: Remote execute 호출 수 조회
    // 입력: 없음
    // 출력: dispatch attempt 수
    [[nodiscard]] int callCount() const
    {
        return m_callCount.load(std::memory_order_relaxed);
    }

    // 목적: Worker acceptance callback 호출 수 조회
    // 입력: 없음
    // 출력: accepted job 수
    [[nodiscard]] int acceptedCount() const
    {
        return m_acceptedCount.load(std::memory_order_relaxed);
    }

    // 목적: dispatch handshake blocking 지점 도달 수 조회
    // 입력: 없음
    // 출력: dispatching 관찰 수
    [[nodiscard]] int dispatchingCount() const
    {
        return m_dispatchingCount.load(std::memory_order_relaxed);
    }

    // 목적: accepted execution blocking 지점 도달 수 조회
    // 입력: 없음
    // 출력: running 관찰 수
    [[nodiscard]] int runningCount() const
    {
        return m_runningCount.load(std::memory_order_relaxed);
    }

    // 목적: 종료된 fake Remote execute 수 조회
    // 입력: 없음
    // 출력: terminal return 수
    [[nodiscard]] int finishedCount() const
    {
        return m_finishedCount.load(std::memory_order_relaxed);
    }

    // 목적: 동시에 진행된 Remote handshake high-water 조회
    // 입력: 없음
    // 출력: maximum active handshake 수
    [[nodiscard]] int maximumHandshakes() const
    {
        return m_maxHandshakes.load(std::memory_order_relaxed);
    }

    // 목적: 동시에 accepted 상태였던 Remote execution high-water 조회
    // 입력: 없음
    // 출력: maximum active Remote execution 수
    [[nodiscard]] int maximumExecutions() const
    {
        return m_maxExecutions.load(std::memory_order_relaxed);
    }

private:
    // 목적: atomic high-water counter를 lock 없이 갱신
    // 입력: maximum: 누적 최대값, candidate: 새 관찰값
    // 출력: candidate가 클 때 maximum 갱신
    static void updateMaximum(std::atomic_int& maximum, const int candidate)
    {
        int previous = maximum.load(std::memory_order_relaxed);
        while (candidate > previous && !maximum.compare_exchange_weak(previous, candidate, std::memory_order_relaxed))
        {}
    }

    // 목적: cancellation을 normalized Remote failure로 생성
    // 입력: workerAccepted: acceptance 이전/이후 구분
    // 출력: Cancelled failure result
    [[nodiscard]] static RemoteExportExecutionResult cancelledFailure(const bool workerAccepted)
    {
        return RemoteExportExecutionResult::failure(
            {RemoteExportFailureCode::Cancelled,
             {types::ErrorCode::Cancelled, QStringLiteral("Fake Remote execution cancelled.")},
             {},
             workerAccepted});
    }

    // 목적: scripted outcome을 scheduler가 소비하는 failure code로 변환
    // 입력: step: acceptance/outcome/retryAfter 값
    // 출력: normalized Remote failure
    [[nodiscard]] static RemoteExportFailure makeFailure(const RemoteStep& step)
    {
        RemoteExportFailureCode code = RemoteExportFailureCode::ConnectionFailed;
        switch (step.outcome)
        {
        case RemoteOutcome::Ineligible:
            code = RemoteExportFailureCode::Ineligible;
            break;
        case RemoteOutcome::ServerBusy:
            code = RemoteExportFailureCode::ServerBusy;
            break;
        case RemoteOutcome::ResourceBusy:
            code = RemoteExportFailureCode::ResourceBusy;
            break;
        case RemoteOutcome::ConnectionFailed:
            code = RemoteExportFailureCode::ConnectionFailed;
            break;
        case RemoteOutcome::ConnectionLost:
            code = RemoteExportFailureCode::ConnectionLost;
            break;
        case RemoteOutcome::TimedOut:
            code = RemoteExportFailureCode::TimedOut;
            break;
        case RemoteOutcome::ProtocolViolation:
            code = RemoteExportFailureCode::ProtocolViolation;
            break;
        case RemoteOutcome::RenderFailed:
            code = RemoteExportFailureCode::RenderFailed;
            break;
        case RemoteOutcome::Success:
            break;
        }
        return {code,
                {types::ErrorCode::Unknown, QStringLiteral("Scripted Remote failure.")},
                step.retryAfter,
                step.accepted};
    }

    std::vector<RemoteStep> m_steps;
    mutable QSemaphore m_handshakeRelease;
    mutable QSemaphore m_executionRelease;
    mutable std::atomic_int m_callCount{0};
    mutable std::atomic_int m_acceptedCount{0};
    mutable std::atomic_int m_dispatchingCount{0};
    mutable std::atomic_int m_runningCount{0};
    mutable std::atomic_int m_finishedCount{0};
    mutable std::atomic_int m_activeHandshakes{0};
    mutable std::atomic_int m_maxHandshakes{0};
    mutable std::atomic_int m_activeExecutions{0};
    mutable std::atomic_int m_maxExecutions{0};
};

class MutableSystemMemoryProbe final : public platform::ISystemMemoryProbe
{
public:
    // 목적: test가 변경할 수 있는 available memory snapshot 생성
    // 입력: availableBytes: 초기 available physical memory
    // 출력: deterministic memory probe
    explicit MutableSystemMemoryProbe(const std::uint64_t availableBytes) : m_availableBytes(availableBytes) {}

    // 목적: 현재 configured memory snapshot 반환
    // 입력: 없음
    // 출력: 64 GiB total과 atomic available 값
    [[nodiscard]] std::optional<platform::SystemMemorySnapshot> snapshot() const override
    {
        return platform::SystemMemorySnapshot{64ULL * 1024 * 1024 * 1024,
                                              m_availableBytes.load(std::memory_order_relaxed)};
    }

    // 목적: 다음 scheduler resource 재평가에 사용할 available memory 변경
    // 입력: availableBytes: 새 byte 값
    // 출력: atomic snapshot 갱신
    void setAvailableBytes(const std::uint64_t availableBytes)
    {
        m_availableBytes.store(availableBytes, std::memory_order_relaxed);
    }

private:
    std::atomic_uint64_t m_availableBytes{0};
};

TEST(ExportPlacementTest, LocalOnlyNeverCallsRemote)
{
    (void)test::application();
    auto pipeline = std::make_unique<ScriptedExportPipeline>(3);
    ScriptedExportPipeline* const local = pipeline.get();
    auto remotePort = std::make_unique<ScriptedRemotePort>(std::vector<RemoteStep>(3));
    ScriptedRemotePort* const remote = remotePort.get();
    ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration());
    ExportObservation observation;
    observe(orchestrator, observation);

    ASSERT_TRUE(
        orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::LocalOnly)).hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));

    EXPECT_EQ(3, local->callCount());
    EXPECT_EQ(0, remote->callCount());
    EXPECT_EQ(3, observation.completed->report.succeededCount);
    EXPECT_EQ(3U, observation.completed->report.scheduling.localExecuted);
}

TEST(ExportPlacementTest, RemoteOnlyNeverFallsBackToLocal)
{
    (void)test::application();
    auto pipeline = std::make_unique<ScriptedExportPipeline>(2);
    ScriptedExportPipeline* const local = pipeline.get();
    auto remotePort = std::make_unique<ScriptedRemotePort>(std::vector<RemoteStep>(2));
    ScriptedRemotePort* const remote = remotePort.get();
    ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration());
    ExportObservation observation;
    observe(orchestrator, observation);

    ASSERT_TRUE(
        orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::RemoteOnly)).hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));

    EXPECT_EQ(0, local->callCount());
    EXPECT_EQ(2, remote->callCount());
    EXPECT_EQ(2, observation.completed->report.succeededCount);
    EXPECT_EQ(2U, observation.completed->report.scheduling.remoteExecuted);
}

TEST(ExportPlacementTest, AutoWithoutUsableRemoteProfileContinuesLocally)
{
    (void)test::application();
    auto pipeline = std::make_unique<ScriptedExportPipeline>(1);
    ScriptedExportPipeline* const local = pipeline.get();
    auto remotePort = std::make_unique<ScriptedRemotePort>(std::vector<RemoteStep>(1));
    ScriptedRemotePort* const remote = remotePort.get();
    ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration());
    ExportObservation observation;
    observe(orchestrator, observation);
    ExportPlacementOptions placement = makePlacement(ExportPlacementPolicy::Auto);
    placement.remoteTarget->host.clear();

    ASSERT_TRUE(orchestrator.submitExport(makeBatchRequest(), std::move(placement)).hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));

    EXPECT_EQ(1, local->callCount());
    EXPECT_EQ(0, remote->callCount());
    EXPECT_EQ(1, observation.completed->report.succeededCount);
}

TEST(ExportPlacementTest, AutoUsesLocalAndRemoteAtTheSameTime)
{
    (void)test::application();
    auto pipeline = std::make_unique<ScriptedExportPipeline>(2, LocalBehavior::BlockUntilReleased);
    ScriptedExportPipeline* const local = pipeline.get();
    auto remotePort =
        std::make_unique<ScriptedRemotePort>(std::vector<RemoteStep>{{RemoteOutcome::Success, true, false, true, {}}});
    ScriptedRemotePort* const remote = remotePort.get();
    ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration(1, 1));
    ExportObservation observation;
    observe(orchestrator, observation);

    ASSERT_TRUE(orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::Auto)).hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return local->callCount() == 1 && remote->runningCount() == 1; }));
    EXPECT_EQ(1, local->maximumActive());
    EXPECT_EQ(1, remote->maximumExecutions());

    local->release();
    remote->releaseExecution();
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));
    EXPECT_EQ(2, observation.completed->report.succeededCount);
    EXPECT_EQ(1U, observation.completed->report.scheduling.localExecuted);
    EXPECT_EQ(1U, observation.completed->report.scheduling.remoteExecuted);
}

TEST(ExportPlacementTest, RemoteDispatchHandshakeIsSerializedWhileAcceptedJobsOverlap)
{
    (void)test::application();
    auto pipeline = std::make_unique<ScriptedExportPipeline>(2);
    auto remotePort = std::make_unique<ScriptedRemotePort>(std::vector<RemoteStep>{
        {RemoteOutcome::Success, true, true, true, {}}, {RemoteOutcome::Success, true, true, true, {}}});
    ScriptedRemotePort* const remote = remotePort.get();
    ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration(1, 2));
    ExportObservation observation;
    observe(orchestrator, observation);

    ASSERT_TRUE(
        orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::RemoteOnly)).hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return remote->dispatchingCount() == 1; }));
    QThread::msleep(20);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    EXPECT_EQ(1, remote->callCount());

    remote->releaseHandshake();
    ASSERT_TRUE(waitForCondition([&]() { return remote->dispatchingCount() == 2; }));
    EXPECT_EQ(1, remote->maximumHandshakes());
    remote->releaseHandshake();
    ASSERT_TRUE(waitForCondition([&]() { return remote->runningCount() == 2; }));
    EXPECT_EQ(2, remote->maximumExecutions());

    remote->releaseExecution(2);
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));
    EXPECT_EQ(2, observation.completed->report.succeededCount);
}

TEST(ExportPlacementTest, AutoFallsBackLocallyOnlyForNotStartedRemoteFailures)
{
    (void)test::application();
    const std::vector<RemoteStep> steps{{RemoteOutcome::Ineligible, false, false, false, {}},
                                        {RemoteOutcome::ServerBusy, false, false, false, {}},
                                        {RemoteOutcome::ResourceBusy, true, false, false, 1ms},
                                        {RemoteOutcome::ConnectionFailed, false, false, false, {}}};

    for (const RemoteStep& step : steps)
    {
        SCOPED_TRACE(static_cast<int>(step.outcome));
        auto pipeline = std::make_unique<ScriptedExportPipeline>(1);
        ScriptedExportPipeline* const local = pipeline.get();
        auto remotePort = std::make_unique<ScriptedRemotePort>(std::vector<RemoteStep>{step});
        ScriptedRemotePort* const remote = remotePort.get();
        ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration(1, 1));
        ExportObservation observation;
        observe(orchestrator, observation);

        ASSERT_TRUE(
            orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::Auto)).hasValue());
        ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));
        EXPECT_EQ(1, remote->callCount());
        EXPECT_EQ(1, local->callCount());
        EXPECT_EQ(1, observation.completed->report.succeededCount);
    }
}

TEST(ExportPlacementTest, AutoDoesNotRepeatUnknownOutcomeLocally)
{
    (void)test::application();
    auto pipeline = std::make_unique<ScriptedExportPipeline>(1);
    ScriptedExportPipeline* const local = pipeline.get();
    auto remotePort = std::make_unique<ScriptedRemotePort>(
        std::vector<RemoteStep>{{RemoteOutcome::TimedOut, true, false, false, {}}});
    ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration(1, 1));
    ExportObservation observation;
    observe(orchestrator, observation);

    ASSERT_TRUE(orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::Auto)).hasValue());
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));

    EXPECT_EQ(0, local->callCount());
    ASSERT_EQ(1, observation.completed->report.failedCount);
    ASSERT_EQ(1, observation.completed->report.items.size());
    EXPECT_EQ(ExportItemFailureKind::Ambiguous, observation.completed->report.items.constFirst().failureKind);
    EXPECT_EQ(1U, observation.completed->report.scheduling.ambiguous);
}

TEST(ExportPlacementTest, CancelsRemoteDispatchingAndRunningWithoutRequeue)
{
    (void)test::application();
    for (const bool cancelAfterAcceptance : {false, true})
    {
        SCOPED_TRACE(cancelAfterAcceptance);
        auto pipeline = std::make_unique<ScriptedExportPipeline>(1);
        ScriptedExportPipeline* const local = pipeline.get();
        const RemoteStep step{
            RemoteOutcome::Success, cancelAfterAcceptance, !cancelAfterAcceptance, cancelAfterAcceptance, {}};
        auto remotePort = std::make_unique<ScriptedRemotePort>(std::vector<RemoteStep>{step});
        ScriptedRemotePort* const remote = remotePort.get();
        ExportOrchestrator orchestrator(std::move(pipeline), std::move(remotePort), nullptr, makeConfiguration(1, 1));
        ExportObservation observation;
        observe(orchestrator, observation);

        const ExportSubmissionResult submitted =
            orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::Auto));
        ASSERT_TRUE(submitted.hasValue());
        ASSERT_TRUE(waitForCondition(
            [&]() { return cancelAfterAcceptance ? remote->runningCount() == 1 : remote->dispatchingCount() == 1; }));
        EXPECT_TRUE(orchestrator.cancelExport(submitted.value()));
        ASSERT_TRUE(waitForCondition([&]() { return remote->finishedCount() == 1; }));
        EXPECT_EQ(1U, observation.cancelled.size());
        EXPECT_FALSE(observation.completed.has_value());
        EXPECT_EQ(0, local->callCount());
    }
}

TEST(ExportPlacementTest, LocalQueueKeepsStableOrderAndSlotLimit)
{
    (void)test::application();
    auto pipeline = std::make_unique<ScriptedExportPipeline>(3, LocalBehavior::BlockUntilReleased);
    ScriptedExportPipeline* const local = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline), 1);
    ExportObservation observation;
    observe(orchestrator, observation);

    ASSERT_TRUE(
        orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::LocalOnly)).hasValue());
    for (int expected = 1; expected <= 3; ++expected)
    {
        ASSERT_TRUE(waitForCondition([&]() { return local->callCount() == expected; }));
        local->release();
    }
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }));

    EXPECT_EQ(1, local->maximumActive());
    EXPECT_EQ((std::vector<QString>{
                  QStringLiteral("item-0.arw"), QStringLiteral("item-1.arw"), QStringLiteral("item-2.arw")}),
              local->startOrder());
}

TEST(ExportPlacementTest, LocalResourceReserveDefersInsteadOfOvercommitting)
{
    (void)test::application();
    constexpr std::uint64_t GiB = 1024ULL * 1024 * 1024;
    MutableSystemMemoryProbe memoryProbe(10 * GiB);
    ExportSchedulingConfiguration configuration = makeConfiguration(4, 1);
    configuration.enforceLocalResourceReserve = true;
    configuration.reservedLogicalProcessors = 0;
    configuration.memoryReserveBytes = 10 * GiB;
    configuration.memoryClaimPerJobBytes = 1 * GiB;
    auto pipeline = std::make_unique<ScriptedExportPipeline>(1);
    ScriptedExportPipeline* const local = pipeline.get();
    ExportOrchestrator orchestrator(std::move(pipeline), {}, &memoryProbe, configuration);
    ExportObservation observation;
    observe(orchestrator, observation);

    ASSERT_TRUE(
        orchestrator.submitExport(makeBatchRequest(), makePlacement(ExportPlacementPolicy::LocalOnly)).hasValue());
    QThread::msleep(50);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    EXPECT_EQ(0, local->callCount());

    memoryProbe.setAvailableBytes(12 * GiB);
    ASSERT_TRUE(waitForCondition([&]() { return observation.completed.has_value(); }, 3000));
    EXPECT_EQ(1, local->callCount());
}

}  // namespace
}  // namespace flexraw::core::orchestration

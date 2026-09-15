#include <atomic>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <QSemaphore>
#include <QThread>

#include <gtest/gtest.h>

#include "job_scheduler.h"

namespace flexraw::worker::runtime
{
namespace
{

class BlockingRenderJobRunner final : public IRenderJobRunner
{
public:
    // 목적: job 시작을 알리고 test가 release하거나 cancellation할 때까지 대기
    // 입력: request: 결과 artifact path source, cancellationToken: scheduler cancellation state
    // 출력: release 시 성공, cancellation 시 Cancelled failure
    [[nodiscard]] RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        m_executeCount.fetch_add(1, std::memory_order_relaxed);
        {
            const std::scoped_lock lock(m_startedPathsMutex);
            m_startedPaths.push_back(request.sourcePath);
        }
        m_started.release();
        while (!m_release.tryAcquire(1, 5))
        {
            if (cancellationToken.isCancellationRequested())
            {
                return RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
                    {{core::types::ErrorCode::Cancelled, QStringLiteral("test cancellation")}, {}}));
            }
        }
        if (cancellationToken.isCancellationRequested())
        {
            return RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
                {{core::types::ErrorCode::Cancelled, QStringLiteral("test cancellation")}, {}}));
        }
        return RenderJobExecutionResult::success(
            core::render::ResolvedRenderPipelineResult::success({{request.outputPath, 42}, {}}));
    }

    // 목적: 지정 수의 job이 runner에 진입할 때까지 bounded wait
    // 입력: count: 기다릴 시작 event 수, timeoutMilliseconds: 최대 대기 시간
    // 출력: timeout 전에 모두 시작했으면 true
    [[nodiscard]] bool waitForStarted(const int count, const int timeoutMilliseconds = 3000) const
    {
        return m_started.tryAcquire(count, timeoutMilliseconds);
    }

    // 목적: 대기 중인 runner invocation을 지정 수만큼 성공 경로로 진행
    // 입력: count: release token 수
    // 출력: 없음
    void release(const int count) const
    {
        m_release.release(count);
    }

    // 목적: 실제 runner 진입 횟수 조회
    // 입력: 없음
    // 출력: queued cancellation을 제외한 실행 횟수
    [[nodiscard]] int executeCount() const noexcept
    {
        return m_executeCount.load(std::memory_order_relaxed);
    }

    // 목적: runner가 관찰한 job 시작 순서의 thread-safe copy 반환
    // 입력: 없음
    // 출력: request source path 기준 시작 순서
    [[nodiscard]] std::vector<QString> startedPaths() const
    {
        const std::scoped_lock lock(m_startedPathsMutex);
        return m_startedPaths;
    }

private:
    mutable QSemaphore m_started;
    mutable QSemaphore m_release;
    mutable std::atomic_int m_executeCount{0};
    mutable std::mutex m_startedPathsMutex;
    mutable std::vector<QString> m_startedPaths;
};

class ThrowingRenderJobRunner final : public IRenderJobRunner
{
public:
    // 목적: scheduler exception boundary test를 위해 고정 예외 발생
    // 입력: request/cancellationToken: 사용하지 않는 runner contract 값
    // 출력: 없음; runtime_error 예외
    [[nodiscard]] RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        static_cast<void>(request);
        static_cast<void>(cancellationToken);
        throw std::runtime_error("runner failed");
    }
};

class ResourceBusyRenderJobRunner final : public IRenderJobRunner
{
public:
    // 목적: resource admission 거절의 terminal status 분류 검증
    // 입력: request/cancellationToken: 사용하지 않는 runner contract 값
    // 출력: 고정 RenderResourceBusy failure
    [[nodiscard]] RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        static_cast<void>(request);
        static_cast<void>(cancellationToken);
        return RenderJobExecutionResult::failure({QStringLiteral("resource busy"), {}});
    }
};

class CompletionCollector final
{
public:
    // 목적: concurrent scheduler callback의 terminal outcome 저장
    // 입력: outcome: job identity/path와 terminal result
    // 출력: 없음
    void collect(RenderJobOutcome outcome)
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_outcomes.push_back(std::move(outcome));
        }
        m_completed.release();
    }

    // 목적: 지정 수의 completion callback을 bounded wait
    // 입력: count: 기다릴 callback 수, timeoutMilliseconds: 최대 대기 시간
    // 출력: timeout 전에 모두 도착했으면 true
    [[nodiscard]] bool waitForCompleted(const int count, const int timeoutMilliseconds = 3000)
    {
        return m_completed.tryAcquire(count, timeoutMilliseconds);
    }

    // 목적: 지금까지 수집한 terminal outcome의 thread-safe copy 반환
    // 입력: 없음
    // 출력: callback 도착 순서의 outcome 목록
    [[nodiscard]] std::vector<RenderJobOutcome> outcomes() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_outcomes;
    }

private:
    mutable std::mutex m_mutex;
    QSemaphore m_completed;
    std::vector<RenderJobOutcome> m_outcomes;
};

class StatusCollector final
{
public:
    // 목적: concurrent scheduler callback의 lifecycle status 저장
    // 입력: status: accepted job identity와 신규 state
    // 출력: 없음
    void collect(RenderJobStatus status)
    {
        const std::scoped_lock lock(m_mutex);
        m_statuses.push_back(std::move(status));
    }

    // 목적: 지금까지 수집한 lifecycle status의 thread-safe copy 반환
    // 입력: 없음
    // 출력: callback 발행 순서의 status 목록
    [[nodiscard]] std::vector<RenderJobStatus> statuses() const
    {
        const std::scoped_lock lock(m_mutex);
        return m_statuses;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<RenderJobStatus> m_statuses;
};

class RecordingPipeline final : public core::render::IResolvedRenderPipeline
{
public:
    // 목적: PipelineRenderJobRunner의 request/token 위임을 기록
    // 입력: request: 관찰할 resolved path, cancellationToken: 관찰할 state
    // 출력: 고정 성공 artifact
    [[nodiscard]] core::render::ResolvedRenderPipelineResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        called = true;
        sourcePath = request.sourcePath;
        cancelled = cancellationToken.isCancellationRequested();
        return core::render::ResolvedRenderPipelineResult::success({{QStringLiteral("result.jpg"), 7}, {}});
    }

    mutable bool called{false};
    mutable bool cancelled{false};
    mutable QString sourcePath;
};

// 목적: scheduler test용 최소 resolved render job 생성
// 입력: jobId: Runtime identity 값, sessionId: connection identity
// 출력: identity와 구분 가능한 source/output path를 가진 job
[[nodiscard]] ScheduledRenderJob makeJob(const std::uint64_t jobId, const WorkerSessionId sessionId = 1)
{
    const QString suffix = QString::number(jobId);
    return {{sessionId, {jobId}},
            QStringLiteral("exports/output-%1.jpg").arg(suffix),
            {QStringLiteral("source-%1.CR3").arg(suffix), QStringLiteral("output-%1.jpg").arg(suffix), {}, {}}};
}

// 목적: collector에 terminal outcome을 전달하는 copyable callback 생성
// 입력: collector: test lifetime 동안 유지할 sink
// 출력: scheduler completion callback
[[nodiscard]] RenderJobCompletion collectInto(CompletionCollector& collector)
{
    return [&collector](RenderJobOutcome outcome) { collector.collect(std::move(outcome)); };
}

// 목적: collector에 lifecycle status를 전달하는 copyable callback 생성
// 입력: collector: test lifetime 동안 유지할 sink
// 출력: scheduler status callback
[[nodiscard]] RenderJobStatusCallback collectStatusesInto(StatusCollector& collector)
{
    return [&collector](RenderJobStatus status) { collector.collect(std::move(status)); };
}

TEST(JobSchedulerStatusTest, EmitsQueuedLifecycleInAuthoritativeFifoOrder)
{
    BlockingRenderJobRunner runner;
    CompletionCollector completions;
    StatusCollector statuses;
    JobScheduler scheduler(runner, 1, 1, collectStatusesInto(statuses));

    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(completions)));
    ASSERT_TRUE(runner.waitForStarted(1));
    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(2), collectInto(completions)));

    runner.release(1);
    ASSERT_TRUE(completions.waitForCompleted(1));
    ASSERT_TRUE(runner.waitForStarted(1));
    runner.release(1);
    ASSERT_TRUE(completions.waitForCompleted(1));

    const std::vector<RenderJobStatus> observed = statuses.statuses();
    ASSERT_EQ(5U, observed.size());
    EXPECT_EQ(1U, observed[0].key.jobId.value);
    EXPECT_EQ(RenderJobState::Running, observed[0].state);
    EXPECT_EQ(2U, observed[1].key.jobId.value);
    EXPECT_EQ(RenderJobState::Queued, observed[1].state);
    EXPECT_EQ(1U, observed[2].key.jobId.value);
    EXPECT_EQ(RenderJobState::Succeeded, observed[2].state);
    EXPECT_EQ(2U, observed[3].key.jobId.value);
    EXPECT_EQ(RenderJobState::Running, observed[3].state);
    EXPECT_EQ(2U, observed[4].key.jobId.value);
    EXPECT_EQ(RenderJobState::Succeeded, observed[4].state);
}

TEST(JobSchedulerStatusTest, EmitsCancellationOnlyAtTerminalTransition)
{
    BlockingRenderJobRunner runner;
    CompletionCollector completions;
    StatusCollector statuses;
    JobScheduler scheduler(runner, 1, 1, collectStatusesInto(statuses));

    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(completions)));
    ASSERT_TRUE(runner.waitForStarted(1));
    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(2), collectInto(completions)));

    ASSERT_TRUE(scheduler.cancel({1, {2}}));
    ASSERT_TRUE(completions.waitForCompleted(1));
    ASSERT_TRUE(scheduler.cancel({1, {1}}));
    ASSERT_TRUE(completions.waitForCompleted(1));

    const std::vector<RenderJobStatus> observed = statuses.statuses();
    ASSERT_EQ(4U, observed.size());
    EXPECT_EQ(RenderJobState::Running, observed[0].state);
    EXPECT_EQ(RenderJobState::Queued, observed[1].state);
    EXPECT_EQ(2U, observed[2].key.jobId.value);
    EXPECT_EQ(RenderJobState::Cancelled, observed[2].state);
    EXPECT_EQ(1U, observed[3].key.jobId.value);
    EXPECT_EQ(RenderJobState::Cancelled, observed[3].state);
}

TEST(JobSchedulerStatusTest, MapsResourceBusyAndRunnerExceptionToFailed)
{
    StatusCollector busyStatuses;
    CompletionCollector busyCompletions;
    ResourceBusyRenderJobRunner busyRunner;
    JobScheduler busyScheduler(busyRunner, 1, 0, collectStatusesInto(busyStatuses));
    ASSERT_EQ(SubmitStatus::Accepted, busyScheduler.submit(makeJob(1), collectInto(busyCompletions)));
    ASSERT_TRUE(busyCompletions.waitForCompleted(1));

    const std::vector<RenderJobStatus> busyObserved = busyStatuses.statuses();
    ASSERT_EQ(2U, busyObserved.size());
    EXPECT_EQ(RenderJobState::Running, busyObserved[0].state);
    EXPECT_EQ(RenderJobState::Failed, busyObserved[1].state);

    StatusCollector exceptionStatuses;
    CompletionCollector exceptionCompletions;
    ThrowingRenderJobRunner throwingRunner;
    JobScheduler exceptionScheduler(throwingRunner, 1, 0, collectStatusesInto(exceptionStatuses));
    ASSERT_EQ(SubmitStatus::Accepted, exceptionScheduler.submit(makeJob(2), collectInto(exceptionCompletions)));
    ASSERT_TRUE(exceptionCompletions.waitForCompleted(1));

    const std::vector<RenderJobStatus> exceptionObserved = exceptionStatuses.statuses();
    ASSERT_EQ(2U, exceptionObserved.size());
    EXPECT_EQ(RenderJobState::Running, exceptionObserved[0].state);
    EXPECT_EQ(RenderJobState::Failed, exceptionObserved[1].state);
}

TEST(JobSchedulerStatusTest, IsolatesCallbackExceptionsAndAllowsSnapshotReentry)
{
    BlockingRenderJobRunner runner;
    CompletionCollector completions;
    std::atomic_int callbackCount{0};
    JobScheduler* schedulerAddress = nullptr;
    JobScheduler scheduler(runner, 1, 0, [&](RenderJobStatus status) {
        static_cast<void>(status);
        static_cast<void>(schedulerAddress->snapshot());
        if (callbackCount.fetch_add(1, std::memory_order_relaxed) == 0)
        {
            throw std::runtime_error("status sink failed");
        }
    });
    schedulerAddress = &scheduler;

    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(completions)));
    ASSERT_TRUE(runner.waitForStarted(1));
    runner.release(1);
    ASSERT_TRUE(completions.waitForCompleted(1));

    EXPECT_EQ(2, callbackCount.load(std::memory_order_relaxed));
}

TEST(JobSchedulerStatusTest, DeliversSnapshotSafeCallbacksOnCallerAndWorkerContexts)
{
    BlockingRenderJobRunner runner;
    CompletionCollector completions;
    std::mutex deliveryMutex;
    std::vector<std::pair<RenderJobState, QThread*>> deliveries;
    JobScheduler* schedulerAddress = nullptr;
    JobScheduler scheduler(runner, 1, 0, [&](RenderJobStatus status) {
        static_cast<void>(schedulerAddress->snapshot());
        const std::scoped_lock lock(deliveryMutex);
        deliveries.emplace_back(status.state, QThread::currentThread());
    });
    schedulerAddress = &scheduler;
    QThread* const callerThread = QThread::currentThread();

    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(completions)));
    ASSERT_TRUE(runner.waitForStarted(1));
    {
        const std::scoped_lock lock(deliveryMutex);
        ASSERT_EQ(1U, deliveries.size());
        EXPECT_EQ(RenderJobState::Running, deliveries.front().first);
        EXPECT_EQ(callerThread, deliveries.front().second);
    }

    runner.release(1);
    ASSERT_TRUE(completions.waitForCompleted(1));
    {
        const std::scoped_lock lock(deliveryMutex);
        ASSERT_EQ(2U, deliveries.size());
        EXPECT_EQ(RenderJobState::Succeeded, deliveries.back().first);
        EXPECT_NE(callerThread, deliveries.back().second);
    }
}

TEST(JobSchedulerTest, EnforcesRunningAndQueueBoundsWithHighWaterStats)
{
    BlockingRenderJobRunner runner;
    JobScheduler scheduler(runner, 2, 2);
    CompletionCollector collector;

    for (std::uint64_t jobId = 1; jobId <= 4; ++jobId)
    {
        EXPECT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(jobId), collectInto(collector)));
    }
    EXPECT_EQ(SubmitStatus::QueueFull, scheduler.submit(makeJob(5), collectInto(collector)));
    ASSERT_TRUE(runner.waitForStarted(2));

    WorkerRuntimeSnapshot snapshot = scheduler.snapshot();
    EXPECT_EQ(2U, snapshot.running);
    EXPECT_EQ(2U, snapshot.queued);
    EXPECT_EQ(2U, snapshot.highWater.runningHighWater);
    EXPECT_EQ(2U, snapshot.highWater.queuedHighWater);

    runner.release(2);
    ASSERT_TRUE(collector.waitForCompleted(2));
    ASSERT_TRUE(runner.waitForStarted(2));
    runner.release(2);
    ASSERT_TRUE(collector.waitForCompleted(2));

    snapshot = scheduler.snapshot();
    EXPECT_EQ(0U, snapshot.running);
    EXPECT_EQ(0U, snapshot.queued);
    EXPECT_EQ(2U, snapshot.highWater.runningHighWater);
    EXPECT_EQ(2U, snapshot.highWater.queuedHighWater);
    EXPECT_EQ(4U, collector.outcomes().size());
}

TEST(JobSchedulerTest, RejectsZeroAndDuplicateActiveJobIds)
{
    BlockingRenderJobRunner runner;
    CompletionCollector collector;
    StatusCollector statuses;
    JobScheduler scheduler(runner, 1, 1, collectStatusesInto(statuses));

    EXPECT_EQ(SubmitStatus::InvalidJobId, scheduler.submit(makeJob(0), collectInto(collector)));
    EXPECT_EQ(SubmitStatus::InvalidJobId, scheduler.submit(makeJob(1, 0), collectInto(collector)));
    EXPECT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(collector)));
    ASSERT_TRUE(runner.waitForStarted(1));
    EXPECT_EQ(SubmitStatus::DuplicateJobId, scheduler.submit(makeJob(1), collectInto(collector)));

    EXPECT_TRUE(scheduler.cancel({1, {1}}));
    ASSERT_TRUE(collector.waitForCompleted(1));
    ASSERT_EQ(1U, collector.outcomes().size());
    ASSERT_TRUE(collector.outcomes().front().result.hasValue());
    EXPECT_EQ(core::types::ErrorCode::Cancelled, collector.outcomes().front().result.value().error().cause.code);

    const std::vector<RenderJobStatus> observed = statuses.statuses();
    ASSERT_EQ(2U, observed.size());
    EXPECT_EQ(RenderJobState::Running, observed[0].state);
    EXPECT_EQ(RenderJobState::Cancelled, observed[1].state);
}

TEST(JobSchedulerTest, AllowsSameRuntimeJobIdInDifferentSessions)
{
    BlockingRenderJobRunner runner;
    JobScheduler scheduler(runner, 2, 0);
    CompletionCollector collector;

    EXPECT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1, 11), collectInto(collector)));
    EXPECT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1, 22), collectInto(collector)));
    ASSERT_TRUE(runner.waitForStarted(2));

    EXPECT_TRUE(scheduler.cancel({11, {1}}));
    EXPECT_TRUE(scheduler.cancel({22, {1}}));
    ASSERT_TRUE(collector.waitForCompleted(2));
    EXPECT_EQ(2U, collector.outcomes().size());
}

TEST(JobSchedulerTest, StartsQueuedJobsInFifoOrder)
{
    BlockingRenderJobRunner runner;
    JobScheduler scheduler(runner, 1, 2);
    CompletionCollector collector;
    for (std::uint64_t jobId = 1; jobId <= 3; ++jobId)
    {
        ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(jobId), collectInto(collector)));
    }

    for (int index = 0; index < 3; ++index)
    {
        ASSERT_TRUE(runner.waitForStarted(1));
        runner.release(1);
        ASSERT_TRUE(collector.waitForCompleted(1));
    }

    const std::vector<QString> startedPaths = runner.startedPaths();
    ASSERT_EQ(3U, startedPaths.size());
    EXPECT_EQ(QStringLiteral("source-1.CR3"), startedPaths[0]);
    EXPECT_EQ(QStringLiteral("source-2.CR3"), startedPaths[1]);
    EXPECT_EQ(QStringLiteral("source-3.CR3"), startedPaths[2]);
}

TEST(JobSchedulerTest, CancelsQueuedJobWithoutExecutingItAndCompletesOnce)
{
    BlockingRenderJobRunner runner;
    JobScheduler scheduler(runner, 1, 1);
    CompletionCollector collector;
    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(collector)));
    ASSERT_TRUE(runner.waitForStarted(1));
    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(2), collectInto(collector)));

    EXPECT_TRUE(scheduler.cancel({1, {2}}));
    ASSERT_TRUE(collector.waitForCompleted(1));
    EXPECT_EQ(1, runner.executeCount());
    EXPECT_EQ(0U, scheduler.snapshot().queued);

    EXPECT_TRUE(scheduler.cancel({1, {1}}));
    ASSERT_TRUE(collector.waitForCompleted(1));
    const std::vector<RenderJobOutcome> outcomes = collector.outcomes();
    ASSERT_EQ(2U, outcomes.size());
    EXPECT_NE(outcomes[0].key.jobId.value, outcomes[1].key.jobId.value);
    ASSERT_TRUE(outcomes[0].result.hasValue());
    ASSERT_TRUE(outcomes[1].result.hasValue());
    EXPECT_EQ(core::types::ErrorCode::Cancelled, outcomes[0].result.value().error().cause.code);
    EXPECT_EQ(core::types::ErrorCode::Cancelled, outcomes[1].result.value().error().cause.code);
}

TEST(JobSchedulerTest, ShutdownCancelsQueuedAndRunningJobsThenRejectsSubmit)
{
    BlockingRenderJobRunner runner;
    CompletionCollector collector;
    StatusCollector statuses;
    JobScheduler scheduler(runner, 1, 2, collectStatusesInto(statuses));
    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(collector)));
    ASSERT_TRUE(runner.waitForStarted(1));
    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(2), collectInto(collector)));
    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(3), collectInto(collector)));

    scheduler.shutdown();

    ASSERT_TRUE(collector.waitForCompleted(3));
    const WorkerRuntimeSnapshot snapshot = scheduler.snapshot();
    EXPECT_FALSE(snapshot.accepting);
    EXPECT_EQ(0U, snapshot.running);
    EXPECT_EQ(0U, snapshot.queued);
    EXPECT_EQ(SubmitStatus::ShuttingDown, scheduler.submit(makeJob(4), collectInto(collector)));
    ASSERT_EQ(3U, collector.outcomes().size());
    for (const RenderJobOutcome& outcome : collector.outcomes())
    {
        ASSERT_TRUE(outcome.result.hasValue());
        ASSERT_TRUE(outcome.result.value().hasError());
        EXPECT_EQ(core::types::ErrorCode::Cancelled, outcome.result.value().error().cause.code);
    }

    int runningCount = 0;
    int queuedCount = 0;
    int cancelledCount = 0;
    for (const RenderJobStatus& status : statuses.statuses())
    {
        runningCount += status.state == RenderJobState::Running ? 1 : 0;
        queuedCount += status.state == RenderJobState::Queued ? 1 : 0;
        cancelledCount += status.state == RenderJobState::Cancelled ? 1 : 0;
    }
    EXPECT_EQ(1, runningCount);
    EXPECT_EQ(2, queuedCount);
    EXPECT_EQ(3, cancelledCount);
}

TEST(JobSchedulerTest, ConvertsRunnerExceptionToSingleTerminalFailure)
{
    ThrowingRenderJobRunner runner;
    JobScheduler scheduler(runner, 1, 0);
    CompletionCollector collector;

    ASSERT_EQ(SubmitStatus::Accepted, scheduler.submit(makeJob(1), collectInto(collector)));
    ASSERT_TRUE(collector.waitForCompleted(1));

    const std::vector<RenderJobOutcome> outcomes = collector.outcomes();
    ASSERT_EQ(1U, outcomes.size());
    ASSERT_TRUE(outcomes.front().result.hasValue());
    ASSERT_TRUE(outcomes.front().result.value().hasError());
    EXPECT_EQ(core::types::ErrorCode::Unknown, outcomes.front().result.value().error().cause.code);
    EXPECT_EQ(QStringLiteral("runner failed"), outcomes.front().result.value().error().cause.message);
}

TEST(JobSchedulerTest, RejectsZeroConcurrencyConfiguration)
{
    BlockingRenderJobRunner runner;

    EXPECT_THROW(static_cast<void>(JobScheduler(runner, 0, 1)), std::invalid_argument);
}

TEST(PipelineRenderJobRunnerTest, DelegatesRequestAndCancellationToken)
{
    RecordingPipeline pipeline;
    const PipelineRenderJobRunner runner(pipeline);
    core::render::ResolvedRenderRequest request;
    request.sourcePath = QStringLiteral("source.CR3");
    core::types::CancellationSource cancellation;
    cancellation.requestCancellation();

    const RenderJobExecutionResult result = runner.execute(request, cancellation.token());

    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().hasValue());
    EXPECT_TRUE(pipeline.called);
    EXPECT_TRUE(pipeline.cancelled);
    EXPECT_EQ(request.sourcePath, pipeline.sourcePath);
}

}  // namespace
}  // namespace flexraw::worker::runtime

#include "job_scheduler.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QThreadPool>

#include "high_water_counter.h"
#include "operation_types.h"

namespace flexraw::worker::runtime
{
namespace
{

struct RenderJobKeyHash final
{
    // 목적: session-scoped JobId pair를 unordered active map hash로 결합
    // 입력: key: Worker session과 wire JobId
    // 출력: process-local hash 값
    [[nodiscard]] std::size_t operator()(const RenderJobKey& key) const noexcept
    {
        const std::size_t sessionHash = std::hash<WorkerSessionId>{}(key.sessionId);
        const std::size_t jobHash = std::hash<protocol::JobId>{}(key.jobId);
        return sessionHash ^ (jobHash + 0x9E3779B9U + (sessionHash << 6U) + (sessionHash >> 2U));
    }
};

// 목적: 실행 전 queue에서 취소된 job의 terminal pipeline result 생성
// 입력: 없음
// 출력: timing이 0이고 원인이 Cancelled인 failure result
[[nodiscard]] RenderJobExecutionResult makeQueuedCancellationResult()
{
    return RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
        {{core::types::ErrorCode::Cancelled, QStringLiteral("Queued render job was cancelled.")}, {}}));
}

// 목적: runner가 예외를 던진 경우 scheduler 경계를 지키는 terminal failure 생성
// 입력: message: 예외 또는 fallback 진단
// 출력: timing이 0이고 원인이 Unknown인 failure result
[[nodiscard]] RenderJobExecutionResult makeRunnerExceptionResult(QString message)
{
    return RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
        {{core::types::ErrorCode::Unknown, std::move(message)}, {}}));
}

// 목적: scheduler terminal result를 transport-neutral lifecycle 상태로 분류
// 입력: result: resource admission 또는 resolved render pipeline 결과
// 출력: Succeeded, Failed 또는 Cancelled terminal 상태
[[nodiscard]] RenderJobState terminalStateFor(const RenderJobExecutionResult& result) noexcept
{
    if (result.hasError())
    {
        return RenderJobState::Failed;
    }

    const core::render::ResolvedRenderPipelineResult& pipelineResult = result.value();
    if (pipelineResult.hasValue())
    {
        return RenderJobState::Succeeded;
    }
    if (pipelineResult.error().cause.code == core::types::ErrorCode::Cancelled)
    {
        return RenderJobState::Cancelled;
    }
    return RenderJobState::Failed;
}

}  // namespace

struct JobScheduler::Implementation final
{
    struct JobState final
    {
        ScheduledRenderJob job;
        core::types::CancellationSource cancellation;
        RenderJobCompletion completion;
        bool running{false};
    };

    // 목적: private QThreadPool과 resource 한도를 구성
    // 입력: runner: 동기 실행자, maxConcurrency/queueCapacity: resource 상한, statusCallback: lifecycle sink
    // 출력: 신규 job을 접수할 수 있는 scheduler implementation
    Implementation(const IRenderJobRunner& runner,
                   const std::uint32_t maxConcurrency,
                   const std::uint64_t queueCapacity,
                   RenderJobStatusCallback statusCallback)
        : m_runner(runner),
          m_maxConcurrency(maxConcurrency),
          m_queueCapacity(queueCapacity),
          m_statusCallback(std::move(statusCallback))
    {
        if (maxConcurrency == 0 || maxConcurrency > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        {
            throw std::invalid_argument("Worker max concurrency must fit a positive QThreadPool thread count.");
        }
        m_threadPool.setMaxThreadCount(static_cast<int>(maxConcurrency));
    }

    // 목적: job을 즉시 running 상태로 전환하거나 bounded queue에 삽입
    // 입력: job: session identity와 resolved request, completion: terminal callback
    // 출력: accepted 또는 validation/resource 거절 상태
    [[nodiscard]] SubmitStatus submit(ScheduledRenderJob job, RenderJobCompletion completion)
    {
        if (job.key.sessionId == 0 || job.key.jobId == 0)
        {
            return SubmitStatus::InvalidJobId;
        }

        std::shared_ptr<JobState> state =
            std::make_shared<JobState>(JobState{std::move(job), {}, std::move(completion), false});
        {
            const std::scoped_lock lock(m_mutex);
            if (!m_accepting)
            {
                return SubmitStatus::ShuttingDown;
            }
            if (m_jobs.contains(state->job.key))
            {
                return SubmitStatus::DuplicateJobId;
            }

            if (m_runningCounter.snapshot().current < m_maxConcurrency)
            {
                state->running = true;
                static_cast<void>(m_runningCounter.increment());
            }
            else
            {
                if (m_queuedCounter.snapshot().current >= m_queueCapacity)
                {
                    return SubmitStatus::QueueFull;
                }
                m_queue.push_back(state);
                static_cast<void>(m_queuedCounter.increment());
            }
            m_jobs.emplace(state->job.key, state);
            enqueueStatus({state->job.key, state->running ? RenderJobState::Running : RenderJobState::Queued});
            if (state->running)
            {
                // shutdown이 waitForDone을 통과하기 전에 accepted task를 thread pool에 등록한다.
                launch(state);
            }
        }
        drainStatusEvents();
        return SubmitStatus::Accepted;
    }

    // 목적: active queued/running job을 찾아 cancellation state 갱신
    // 입력: key: Worker session과 wire JobId를 결합한 active identity
    // 출력: cancellation 요청 또는 queued terminal completion을 수행했으면 true
    [[nodiscard]] bool cancel(const RenderJobKey key)
    {
        std::shared_ptr<JobState> cancelledQueuedJob;
        {
            const std::scoped_lock lock(m_mutex);
            const auto jobIterator = m_jobs.find(key);
            if (jobIterator == m_jobs.end())
            {
                return false;
            }

            const std::shared_ptr<JobState>& state = jobIterator->second;
            state->cancellation.requestCancellation();
            if (state->running)
            {
                return true;
            }

            const auto queueIterator = std::find(m_queue.cbegin(), m_queue.cend(), state);
            if (queueIterator != m_queue.cend())
            {
                m_queue.erase(queueIterator);
                m_queuedCounter.decrement();
            }
            cancelledQueuedJob = state;
            m_jobs.erase(jobIterator);
            enqueueStatus({state->job.key, RenderJobState::Cancelled});
        }

        drainStatusEvents();
        invokeCompletion(cancelledQueuedJob, makeQueuedCancellationResult());
        return true;
    }

    // 목적: 신규 접수를 중단하고 모든 active job을 취소한 뒤 QThreadPool 종료 대기
    // 입력: 없음
    // 출력: active running task와 queued callback이 모두 완료된 상태
    void shutdown()
    {
        std::vector<std::shared_ptr<JobState>> cancelledQueuedJobs;
        {
            const std::scoped_lock lock(m_mutex);
            m_accepting = false;
            for (const auto& entry : m_jobs)
            {
                entry.second->cancellation.requestCancellation();
            }

            cancelledQueuedJobs.reserve(m_queue.size());
            while (!m_queue.empty())
            {
                std::shared_ptr<JobState> state = std::move(m_queue.front());
                m_queue.pop_front();
                m_queuedCounter.decrement();
                m_jobs.erase(state->job.key);
                enqueueStatus({state->job.key, RenderJobState::Cancelled});
                cancelledQueuedJobs.push_back(std::move(state));
            }
        }

        drainStatusEvents();
        for (const std::shared_ptr<JobState>& state : cancelledQueuedJobs)
        {
            invokeCompletion(state, makeQueuedCancellationResult());
        }
        m_threadPool.waitForDone();
    }

    // 목적: atomic counters와 accepting flag에서 scheduler 상태 snapshot 생성
    // 입력: 없음
    // 출력: 현재 queue/running, lifetime high-water와 접수 여부
    [[nodiscard]] WorkerRuntimeSnapshot snapshot() const
    {
        const std::scoped_lock lock(m_mutex);
        const core::measurement::HighWaterSnapshot queued = m_queuedCounter.snapshot();
        const core::measurement::HighWaterSnapshot running = m_runningCounter.snapshot();
        return {queued.current, running.current, {queued.highWater, running.highWater}, m_accepting};
    }

private:
    // 목적: accepted running job을 private QThreadPool에서 실행
    // 입력: state: request, cancellation과 completion을 보유한 active state
    // 출력: 없음; terminal result는 finish에서 callback으로 전달
    void launch(const std::shared_ptr<JobState>& state)
    {
        m_threadPool.start([this, state]() {
            RenderJobExecutionResult result = [&]() {
                try
                {
                    return m_runner.execute(state->job.request, state->cancellation.token());
                }
                catch (const std::exception& exception)
                {
                    return makeRunnerExceptionResult(QString::fromUtf8(exception.what()));
                }
                catch (...)
                {
                    return makeRunnerExceptionResult(QStringLiteral("Render job runner threw an unknown exception."));
                }
            }();
            finish(state, std::move(result));
        });
    }

    // 목적: running job을 active map에서 제거하고 queue의 다음 job을 승격
    // 입력: state: 완료한 active state, result: runner terminal result
    // 출력: 없음; callback 정확히 1회 호출 후 다음 job 실행
    void finish(const std::shared_ptr<JobState>& state, RenderJobExecutionResult result)
    {
        const RenderJobState terminalState = terminalStateFor(result);
        std::shared_ptr<JobState> nextJob;
        {
            const std::scoped_lock lock(m_mutex);
            const auto jobIterator = m_jobs.find(state->job.key);
            if (jobIterator == m_jobs.end())
            {
                return;
            }
            m_jobs.erase(jobIterator);
            m_runningCounter.decrement();
            enqueueStatus({state->job.key, terminalState});

            if (m_accepting && !m_queue.empty())
            {
                nextJob = std::move(m_queue.front());
                m_queue.pop_front();
                m_queuedCounter.decrement();
                nextJob->running = true;
                static_cast<void>(m_runningCounter.increment());
                enqueueStatus({nextJob->job.key, RenderJobState::Running});
                // shutdown과 queue 승격 사이에도 미등록 task가 남지 않게 lock 안에서 시작한다.
                launch(nextJob);
            }
        }

        drainStatusEvents();
        invokeCompletion(state, std::move(result));
    }

    // 목적: authoritative state transition 순서대로 status delivery FIFO에 기록
    // 입력: status: accepted job의 신규 lifecycle 상태
    // 출력: 없음; callback은 호출하지 않음
    void enqueueStatus(RenderJobStatus status)
    {
        if (!m_statusCallback)
        {
            return;
        }
        const std::scoped_lock lock(m_statusMutex);
        m_pendingStatuses.push_back(std::move(status));
    }

    // 목적: pending status를 scheduler lock 밖에서 FIFO 순서로 직렬 발행
    // 입력: 없음
    // 출력: 없음; reentrant drain과 callback 예외를 runtime boundary에서 격리
    void drainStatusEvents() noexcept
    {
        if (!m_statusCallback)
        {
            return;
        }
        {
            const std::scoped_lock lock(m_statusMutex);
            if (m_statusDispatching || m_pendingStatuses.empty())
            {
                return;
            }
            m_statusDispatching = true;
        }

        while (true)
        {
            RenderJobStatus status;
            {
                const std::scoped_lock lock(m_statusMutex);
                if (m_pendingStatuses.empty())
                {
                    m_statusDispatching = false;
                    return;
                }
                status = std::move(m_pendingStatuses.front());
                m_pendingStatuses.pop_front();
            }

            try
            {
                m_statusCallback(std::move(status));
            }
            catch (...)
            {}
        }
    }

    // 목적: client callback 예외가 QThreadPool worker를 종료시키지 않도록 terminal outcome 전달
    // 입력: state: job identity/path/callback, result: terminal pipeline 결과
    // 출력: 없음; callback 예외는 scheduler boundary에서 격리
    static void invokeCompletion(const std::shared_ptr<JobState>& state, RenderJobExecutionResult result) noexcept
    {
        if (!state->completion)
        {
            return;
        }
        try
        {
            state->completion({state->job.key, state->job.outputRelativePath, std::move(result)});
        }
        catch (...)
        {}
    }

    const IRenderJobRunner& m_runner;
    const std::uint64_t m_maxConcurrency;
    const std::uint64_t m_queueCapacity;
    mutable std::mutex m_mutex;
    QThreadPool m_threadPool;
    std::deque<std::shared_ptr<JobState>> m_queue;
    std::unordered_map<RenderJobKey, std::shared_ptr<JobState>, RenderJobKeyHash> m_jobs;
    core::measurement::HighWaterCounter m_queuedCounter;
    core::measurement::HighWaterCounter m_runningCounter;
    std::mutex m_statusMutex;
    std::deque<RenderJobStatus> m_pendingStatuses;
    RenderJobStatusCallback m_statusCallback;
    bool m_statusDispatching{false};
    bool m_accepting{true};
};

// 목적: 명시된 running 상한과 bounded waiting queue를 가진 scheduler 생성
// 입력: runner: 동기 runner, maxConcurrency/queueCapacity: resource 한도, statusCallback: optional lifecycle sink
// 출력: 독립 QThreadPool을 소유한 scheduler; maxConcurrency가 0이면 invalid_argument
JobScheduler::JobScheduler(const IRenderJobRunner& runner,
                           const std::uint32_t maxConcurrency,
                           const std::uint64_t queueCapacity,
                           RenderJobStatusCallback statusCallback)
    : m_impl(std::make_unique<Implementation>(runner, maxConcurrency, queueCapacity, std::move(statusCallback)))
{}

// 목적: 신규 접수를 중단하고 queued/running job을 취소한 뒤 worker 종료 대기
// 입력: 없음
// 출력: callback이 모두 terminal outcome을 받은 종료 상태
JobScheduler::~JobScheduler()
{
    m_impl->shutdown();
}

// 목적: job을 즉시 실행하거나 bounded queue에 접수
// 입력: job: non-zero session/JobId와 resolved request, completion: 임의 thread 호출을 허용하는 terminal callback
// 출력: 접수 여부와 duplicate/full/shutdown 거절 사유
SubmitStatus JobScheduler::submit(ScheduledRenderJob job, RenderJobCompletion completion)
{
    return m_impl->submit(std::move(job), std::move(completion));
}

// 목적: queued 또는 running job에 cooperative cancellation 요청
// 입력: key: session과 wire JobId를 결합한 active identity
// 출력: active job을 찾아 취소했으면 true
bool JobScheduler::cancel(const RenderJobKey key)
{
    return m_impl->cancel(key);
}

// 목적: 신규 접수를 중단하고 모든 active job을 취소한 뒤 thread pool 종료 대기
// 입력: 없음
// 출력: queued/running이 0이고 후속 submit이 거절되는 상태
void JobScheduler::shutdown()
{
    m_impl->shutdown();
}

// 목적: 현재 queue/running 수와 lifetime high-water 조회
// 입력: 없음
// 출력: thread-safe scheduler 상태 snapshot
WorkerRuntimeSnapshot JobScheduler::snapshot() const
{
    return m_impl->snapshot();
}

}  // namespace flexraw::worker::runtime

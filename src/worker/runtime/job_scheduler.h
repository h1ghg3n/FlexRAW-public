#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include <QString>

#include "job_status_contracts.h"
#include "render_contracts.h"
#include "render_job_identity.h"
#include "render_job_runner.h"
#include "runtime_observation_contracts.h"

namespace flexraw::worker::runtime
{

struct ScheduledRenderJob
{
    RenderJobKey key;
    QString outputRelativePath;
    core::render::ResolvedRenderRequest request;
};

struct RenderJobOutcome
{
    RenderJobKey key;
    QString outputRelativePath;
    RenderJobExecutionResult result;
};

using RenderJobCompletion = std::function<void(RenderJobOutcome)>;

enum class SubmitStatus
{
    Accepted,
    InvalidJobId,
    DuplicateJobId,
    QueueFull,
    ShuttingDown,
};

class JobScheduler final
{
public:
    // 목적: 명시된 running 상한과 bounded waiting queue를 가진 scheduler 생성
    // 입력: runner: 동기 runner, maxConcurrency/queueCapacity: resource 한도, statusCallback: optional lifecycle sink
    // 출력: 독립 QThreadPool을 소유한 scheduler; maxConcurrency가 0이면 invalid_argument
    JobScheduler(const IRenderJobRunner& runner,
                 std::uint32_t maxConcurrency,
                 std::uint64_t queueCapacity,
                 RenderJobStatusCallback statusCallback = {});

    // 목적: 신규 접수를 중단하고 queued/running job을 취소한 뒤 worker 종료 대기
    // 입력: 없음
    // 출력: callback이 모두 terminal outcome을 받은 종료 상태
    ~JobScheduler();

    JobScheduler(const JobScheduler&) = delete;
    JobScheduler& operator=(const JobScheduler&) = delete;

    // 목적: job을 즉시 실행하거나 bounded queue에 접수
    // 입력: job: non-zero session/JobId와 resolved request, completion: 임의 thread 호출을 허용하는 terminal callback
    // 출력: 접수 여부와 duplicate/full/shutdown 거절 사유
    [[nodiscard]] SubmitStatus submit(ScheduledRenderJob job, RenderJobCompletion completion);

    // 목적: queued 또는 running job에 cooperative cancellation 요청
    // 입력: key: session과 wire JobId를 결합한 active identity
    // 출력: active job을 찾아 취소했으면 true
    [[nodiscard]] bool cancel(RenderJobKey key);

    // 목적: 신규 접수를 중단하고 모든 active job을 취소한 뒤 thread pool 종료 대기
    // 입력: 없음; scheduler completion callback 밖의 owner context에서 호출
    // 출력: queued/running이 0이고 후속 submit이 거절되는 상태
    void shutdown();

    // 목적: 현재 queue/running 수와 lifetime high-water 조회
    // 입력: 없음
    // 출력: thread-safe scheduler 상태 snapshot
    [[nodiscard]] WorkerRuntimeSnapshot snapshot() const;

private:
    struct Implementation;
    std::unique_ptr<Implementation> m_impl;
};

}  // namespace flexraw::worker::runtime

#include "render_worker_runtime_facade.h"

#include <utility>

#include "job_scheduler.h"
#include "worker_path_resolver.h"

namespace flexraw::worker::runtime
{

// 목적: 기존 path resolver와 scheduler를 하나의 server-facing Runtime port로 투영
// 입력: resolver/scheduler: facade보다 오래 사는 검증된 concrete Runtime 객체
// 출력: 별도 state나 scheduling authority를 만들지 않는 facade
RenderWorkerRuntimeFacade::RenderWorkerRuntimeFacade(const WorkerPathResolver& resolver,
                                                     JobScheduler& scheduler) noexcept
    : m_resolver(resolver), m_scheduler(scheduler)
{}

// 목적: root-relative command를 resolve한 뒤 기존 scheduler에 그대로 제출
// 입력: command: session job identity와 processing 값, completion: terminal callback
// 출력: path/value 오류 또는 기존 scheduler 접수 상태
RenderWorkerSubmitResult RenderWorkerRuntimeFacade::submit(RenderWorkerCommand command, RenderJobCompletion completion)
{
    WorkerPathResolver::ResolveResult resolved = m_resolver.resolve(command.request);
    if (resolved.hasError())
    {
        return RenderWorkerSubmitResult::failure(resolved.error());
    }

    ScheduledRenderJob job{command.key, std::move(command.request.outputRelativePath), std::move(resolved.value())};
    return RenderWorkerSubmitResult::success(m_scheduler.submit(std::move(job), std::move(completion)));
}

// 목적: 기존 scheduler의 queued/running cancellation 경로에 위임
// 입력: key: session과 job을 결합한 Runtime identity
// 출력: scheduler가 active job을 찾아 취소했으면 true
bool RenderWorkerRuntimeFacade::cancel(const RenderJobKey key)
{
    return m_scheduler.cancel(key);
}

// 목적: 기존 scheduler의 read-only runtime observation을 Server Adapter에 투영
// 입력: 없음
// 출력: current load, configured limit과 접수 상태 snapshot
WorkerRuntimeSnapshot RenderWorkerRuntimeFacade::snapshot() const
{
    return m_scheduler.snapshot();
}

}  // namespace flexraw::worker::runtime

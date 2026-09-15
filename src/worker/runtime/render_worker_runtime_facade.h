#pragma once

#include "render_worker_runtime_port.h"

namespace flexraw::worker::runtime
{

class JobScheduler;
class WorkerPathResolver;

class RenderWorkerRuntimeFacade final : public IRenderWorkerRuntime
{
public:
    // 목적: 기존 path resolver와 scheduler를 하나의 server-facing Runtime port로 투영
    // 입력: resolver/scheduler: facade보다 오래 사는 검증된 concrete Runtime 객체
    // 출력: 별도 state나 scheduling authority를 만들지 않는 facade
    RenderWorkerRuntimeFacade(const WorkerPathResolver& resolver, JobScheduler& scheduler) noexcept;

    // 목적: root-relative command를 resolve한 뒤 기존 scheduler에 그대로 제출
    // 입력: command: session job identity와 processing 값, completion: terminal callback
    // 출력: path/value 오류 또는 기존 scheduler 접수 상태
    [[nodiscard]] RenderWorkerSubmitResult submit(RenderWorkerCommand command, RenderJobCompletion completion) override;

    // 목적: 기존 scheduler의 queued/running cancellation 경로에 위임
    // 입력: key: session과 job을 결합한 Runtime identity
    // 출력: scheduler가 active job을 찾아 취소했으면 true
    [[nodiscard]] bool cancel(RenderJobKey key) override;

    // 목적: 기존 scheduler의 read-only runtime observation을 Server Adapter에 투영
    // 입력: 없음
    // 출력: current load, configured limit과 접수 상태 snapshot
    [[nodiscard]] WorkerRuntimeSnapshot snapshot() const override;

private:
    const WorkerPathResolver& m_resolver;
    JobScheduler& m_scheduler;
};

}  // namespace flexraw::worker::runtime

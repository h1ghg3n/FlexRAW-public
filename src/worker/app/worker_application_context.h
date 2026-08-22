#pragma once

#include <cstdint>
#include <memory>
#include <utility>

#include <QHostAddress>

#include "admission_render_job_runner.h"
#include "job_scheduler.h"
#include "render_job_runner.h"
#include "render_resource_admission.h"
#include "resolved_render_pipeline.h"
#include "system_memory_probe.h"
#include "worker_path_resolver.h"
#include "worker_server.h"

namespace flexraw::worker::app
{

struct WorkerApplicationConfiguration
{
    std::uint32_t maximumConcurrency{1};
    std::uint64_t queueCapacity{2};
    admission::ResourceAdmissionConfiguration admission;
    network::WorkerServerConfiguration server;
};

class WorkerApplicationContext final
{
public:
    // 목적: resolved render pipeline부터 TCP server까지 Worker production object graph 조립
    // 입력: resolver: 검증된 root 경계, configuration: scheduler와 server resource 한도
    // 출력: 아직 listen하지 않는 Worker application context
    WorkerApplicationContext(runtime::WorkerPathResolver resolver, WorkerApplicationConfiguration configuration);

    // 목적: socket 접수를 중단하고 active render를 취소한 뒤 owned worker thread 종료
    // 입력: 없음
    // 출력: 모든 production dependency가 안전한 역순으로 정리된 상태
    ~WorkerApplicationContext();

    WorkerApplicationContext(const WorkerApplicationContext&) = delete;
    WorkerApplicationContext& operator=(const WorkerApplicationContext&) = delete;

    // 목적: 구성된 Worker server를 지정 TCP endpoint에서 시작
    // 입력: address/port: bind endpoint, port 0은 OS-assigned ephemeral port
    // 출력: listen 성공 여부
    [[nodiscard]] bool listen(const QHostAddress& address, quint16 port);

    // 목적: 신규 socket과 render 접수를 중단하고 active job 종료 대기
    // 입력: 없음
    // 출력: server와 scheduler가 shutdown된 상태
    void shutdown();

    // 목적: 실제 bind된 Worker TCP port 조회
    // 입력: 없음
    // 출력: listen 전 0 또는 bound port
    [[nodiscard]] quint16 serverPort() const noexcept;

    // 목적: Worker server의 마지막 listen/socket 오류 조회
    // 입력: 없음
    // 출력: 진단 가능한 Qt Network 오류 text
    [[nodiscard]] QString errorString() const;

    // 목적: Worker runtime의 current load, lifetime high-water와 접수 상태 조회
    // 입력: 없음
    // 출력: 개별 job correctness authority로 사용하지 않는 thread-safe observation snapshot
    [[nodiscard]] runtime::WorkerRuntimeSnapshot runtimeSnapshot() const;

private:
    runtime::WorkerPathResolver m_resolver;
    std::unique_ptr<platform::ISystemMemoryProbe> m_systemMemoryProbe;
    std::unique_ptr<admission::IRenderResourceAdmission> m_resourceAdmission;
    core::render::ResolvedRenderPipeline m_pipeline;
    runtime::PipelineRenderJobRunner m_pipelineRunner;
    admission::AdmissionRenderJobRunner m_runner;
    runtime::JobScheduler m_scheduler;
    network::WorkerServer m_server;
    bool m_shutdown{false};
};

}  // namespace flexraw::worker::app

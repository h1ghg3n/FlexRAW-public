#include "worker_application_context.h"

#include <stdexcept>

#include "current_system_memory_probe.h"
#include "local_memory_admission.h"
#include "resource_router_admission.h"

namespace flexraw::worker::app
{
namespace
{

// 목적: URL 설정 유무에 따라 local reserve 또는 Resource Router admission implementation 선택
// 입력: configuration: process-level claim/timeout/endpoint configuration, memoryProbe: local fallback capability
// 출력: Worker context가 소유할 admission implementation
[[nodiscard]] std::unique_ptr<admission::IRenderResourceAdmission> createResourceAdmission(
    const admission::ResourceAdmissionConfiguration& configuration, const platform::ISystemMemoryProbe& memoryProbe)
{
    if (configuration.resourceRouterUrl.empty())
    {
        return std::make_unique<admission::LocalMemoryAdmission>(memoryProbe, configuration.minimumAvailableMemoryMiB);
    }
    return std::make_unique<admission::ResourceRouterAdmission>(configuration.resourceRouterUrl,
                                                                configuration.resourceRouterRequestTimeout);
}

}  // namespace

// 목적: resolved render pipeline부터 TCP server까지 Worker production object graph 조립
// 입력: resolver: 검증된 root 경계, configuration: scheduler와 server resource 한도
// 출력: 아직 listen하지 않는 Worker application context
WorkerApplicationContext::WorkerApplicationContext(runtime::WorkerPathResolver resolver,
                                                   WorkerApplicationConfiguration configuration)
    : m_resolver(std::move(resolver)),
      m_systemMemoryProbe(platform::createCurrentSystemMemoryProbe()),
      m_resourceAdmission(createResourceAdmission(configuration.admission, *m_systemMemoryProbe)),
      m_pipelineRunner(m_pipeline),
      m_runner(m_pipelineRunner, *m_resourceAdmission, std::move(configuration.admission)),
      m_scheduler(m_runner, configuration.maximumConcurrency, configuration.queueCapacity),
      m_runtimePort(m_resolver, m_scheduler),
      m_server(m_runtimePort, std::move(configuration.server))
{}

// 목적: socket 접수를 중단하고 active render를 취소한 뒤 owned worker thread 종료
// 입력: 없음
// 출력: 모든 production dependency가 안전한 역순으로 정리된 상태
WorkerApplicationContext::~WorkerApplicationContext()
{
    shutdown();
}

// 목적: 구성된 Worker server를 지정 TCP endpoint에서 시작
// 입력: address/port: bind endpoint, port 0은 OS-assigned ephemeral port
// 출력: listen 성공 여부
bool WorkerApplicationContext::listen(const QHostAddress& address, const quint16 port)
{
    return !m_shutdown && m_server.listen(address, port);
}

// 목적: 신규 socket과 render 접수를 중단하고 active job 종료 대기
// 입력: 없음
// 출력: server와 scheduler가 shutdown된 상태
void WorkerApplicationContext::shutdown()
{
    if (m_shutdown)
    {
        return;
    }
    m_shutdown = true;
    m_server.close();
    m_scheduler.shutdown();
}

// 목적: 실제 bind된 Worker TCP port 조회
// 입력: 없음
// 출력: listen 전 0 또는 bound port
quint16 WorkerApplicationContext::serverPort() const noexcept
{
    return m_server.serverPort();
}

// 목적: Worker server의 마지막 listen/socket 오류 조회
// 입력: 없음
// 출력: 진단 가능한 Qt Network 오류 text
QString WorkerApplicationContext::errorString() const
{
    return m_server.errorString();
}

// 목적: Worker runtime의 current load, lifetime high-water와 접수 상태 조회
// 입력: 없음
// 출력: 개별 job correctness authority로 사용하지 않는 thread-safe observation snapshot
runtime::WorkerRuntimeSnapshot WorkerApplicationContext::runtimeSnapshot() const
{
    return m_scheduler.snapshot();
}

}  // namespace flexraw::worker::app

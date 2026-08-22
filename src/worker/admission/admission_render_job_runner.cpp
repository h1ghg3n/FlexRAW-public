#include "admission_render_job_runner.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#include <QUuid>

#include "log.h"

namespace flexraw::worker::admission
{
namespace
{

constexpr std::chrono::milliseconds CancellationMirrorInterval{20};

// 목적: acquire 이후 모든 return/exception 경로에서 lease를 정확히 한 번 해제
// 입력: admission/lease: runner scope보다 오래 사는 adapter와 active lease
// 출력: scope 종료 시 release 결과를 log하는 noexcept RAII guard
class LeaseReleaseGuard final
{
public:
    LeaseReleaseGuard(IRenderResourceAdmission& admission, RenderResourceLease lease)
        : m_admission(admission), m_lease(std::move(lease))
    {}

    LeaseReleaseGuard(const LeaseReleaseGuard&) = delete;
    LeaseReleaseGuard& operator=(const LeaseReleaseGuard&) = delete;

    ~LeaseReleaseGuard()
    {
        const ResourceLeaseReleaseResult release = m_admission.release(m_lease);
        if (!release.released)
        {
            LOG_WARN("worker", "Render resource lease release was not acknowledged: {}", release.diagnostic);
        }
    }

    // 목적: renewal guard가 같은 active lease identity와 expiry를 사용하도록 조회
    // 입력: 없음
    // 출력: release guard가 scope 동안 소유하는 lease reference
    [[nodiscard]] const RenderResourceLease& lease() const noexcept
    {
        return m_lease;
    }

private:
    IRenderResourceAdmission& m_admission;
    RenderResourceLease m_lease;
};

// 목적: scheduler cancellation과 lease renewal loss를 하나의 pipeline token으로 전달
// 입력: admission/lease/ttl/externalToken: active Router lease와 기존 scheduler cancellation state
// 출력: 필요한 경우 background renewal thread를 소유하는 scoped guard
class LeaseRenewalGuard final
{
public:
    LeaseRenewalGuard(IRenderResourceAdmission& admission,
                      RenderResourceLease lease,
                      const std::chrono::seconds ttl,
                      core::types::CancellationToken externalToken)
        : m_admission(admission), m_lease(std::move(lease)), m_ttl(ttl), m_externalToken(std::move(externalToken))
    {
        if (m_lease.expiresAt != std::chrono::system_clock::time_point::max())
        {
            m_thread = std::jthread([this](const std::stop_token stopToken) { renewUntilStopped(stopToken); });
        }
    }

    LeaseRenewalGuard(const LeaseRenewalGuard&) = delete;
    LeaseRenewalGuard& operator=(const LeaseRenewalGuard&) = delete;

    ~LeaseRenewalGuard()
    {
        stop();
    }

    // 목적: underlying pipeline에 전달할 combined cancellation token 제공
    // 입력: 없음
    // 출력: scheduler cancel 또는 renewal failure를 관찰하는 token
    [[nodiscard]] core::types::CancellationToken token() const
    {
        return m_cancellation.token();
    }

    // 목적: renewal thread 종료를 요청하고 join해 lease state 접근을 안정화
    // 입력: 없음
    // 출력: 후속 failure diagnostic 조회가 안전한 상태
    void stop()
    {
        if (m_thread.joinable())
        {
            m_thread.request_stop();
            m_thread.join();
        }
    }

    // 목적: Router renewal failure 때문에 cancellation이 발생했는지 확인
    // 입력: 없음
    // 출력: Router availability 또는 lease terminal failure면 true
    [[nodiscard]] bool hasRenewalFailure() const noexcept
    {
        return m_renewalFailure.load(std::memory_order_acquire);
    }

    // 목적: stopped renewal thread가 기록한 failure diagnostic 반환
    // 입력: 없음
    // 출력: Router decision diagnostic 또는 generic failure text
    [[nodiscard]] const std::string& failureDiagnostic() const noexcept
    {
        return m_failureDiagnostic;
    }

private:
    // 목적: active lease가 끝나기 전에 갱신하고 external cancellation을 mirror
    // 입력: stopToken: runner scope 종료 signal
    // 출력: renewal 실패 시 internal cancellation state를 설정한 뒤 return
    void renewUntilStopped(const std::stop_token stopToken)
    {
        std::chrono::system_clock::time_point nextRenewal = nextRenewalTime();
        while (!stopToken.stop_requested())
        {
            if (m_externalToken.isCancellationRequested())
            {
                m_cancellation.requestCancellation();
                return;
            }
            if (std::chrono::system_clock::now() >= nextRenewal)
            {
                ResourceAdmissionDecision decision = m_admission.renew(m_lease, m_ttl);
                if (!decision.granted())
                {
                    m_failureDiagnostic = decision.diagnostic.empty() ? "Resource Router lease renewal failed."
                                                                      : std::move(decision.diagnostic);
                    m_renewalFailure.store(true, std::memory_order_release);
                    m_cancellation.requestCancellation();
                    return;
                }
                m_lease = std::move(*decision.lease);
                nextRenewal = nextRenewalTime();
                continue;
            }
            std::this_thread::sleep_for(CancellationMirrorInterval);
        }
    }

    // 목적: authoritative lease expiry의 절반 전에 다음 renewal 시각 계산
    // 입력: 없음
    // 출력: 이미 임박한 expiry면 즉시, 아니면 remaining duration의 절반 뒤 시각
    [[nodiscard]] std::chrono::system_clock::time_point nextRenewalTime() const
    {
        const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        if (m_lease.expiresAt <= now)
        {
            return now;
        }
        return now + (m_lease.expiresAt - now) / 2;
    }

    IRenderResourceAdmission& m_admission;
    RenderResourceLease m_lease;
    std::chrono::seconds m_ttl;
    core::types::CancellationToken m_externalToken;
    core::types::CancellationSource m_cancellation;
    std::jthread m_thread;
    std::atomic_bool m_renewalFailure{false};
    std::string m_failureDiagnostic;
};

// 목적: admission decision을 Worker RenderFailed에 사용할 CoreError로 변환
// 입력: decision: local 또는 Router admission operation 결과
// 출력: unavailable은 Unknown, request 오류는 InvalidArgument failure
[[nodiscard]] core::render::ResolvedRenderPipelineResult makeAdmissionFailure(const ResourceAdmissionDecision& decision)
{
    core::types::ErrorCode code = core::types::ErrorCode::Unknown;
    if (decision.status == ResourceAdmissionStatus::InvalidRequest)
    {
        code = core::types::ErrorCode::InvalidArgument;
    }

    const QString message = decision.diagnostic.empty() ? QStringLiteral("Render resource admission was not granted.")
                                                        : QString::fromStdString(decision.diagnostic);
    return core::render::ResolvedRenderPipelineResult::failure({{code, message}, {}});
}

// 목적: admission BUSY를 processing failure와 분리된 Worker runtime terminal 값으로 변환
// 입력: decision: local reserve 또는 Router BUSY 결과
// 출력: frontend가 retry advice를 구조적으로 읽을 수 있는 resource busy 값
[[nodiscard]] runtime::RenderResourceBusy makeResourceBusy(const ResourceAdmissionDecision& decision)
{
    return {decision.diagnostic.empty() ? QStringLiteral("Render resources are currently unavailable.")
                                        : QString::fromStdString(decision.diagnostic),
            decision.retryAfter};
}

// 목적: scheduler가 render 시작 전 취소한 경우 일관된 terminal cancellation result 생성
// 입력: 없음
// 출력: Cancelled CoreError와 empty RenderStats failure
[[nodiscard]] core::render::ResolvedRenderPipelineResult makeCancellationFailure()
{
    return core::render::ResolvedRenderPipelineResult::failure(
        {{core::types::ErrorCode::Cancelled, QStringLiteral("Render job was cancelled before processing started.")},
         {}});
}

// 목적: Resource Router acquire idempotency에 사용할 새 UUID string 생성
// 입력: 없음
// 출력: braces 없는 lowercase UUID text
[[nodiscard]] std::string createRequestId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}

// 목적: runner configuration을 Router/local admission request value로 변환
// 입력: configuration: worker process에서 고정한 admission policy와 declared claim
// 출력: execution 하나에만 사용되는 fresh lease request
[[nodiscard]] ResourceLeaseRequest makeLeaseRequest(const ResourceAdmissionConfiguration& configuration)
{
    return {createRequestId(),
            configuration.resourceRouterClientId,
            {configuration.renderMemoryClaimMiB, configuration.renderCpuCores, configuration.renderRequiresGpu},
            configuration.resourceRouterLeaseTtl};
}

}  // namespace

// 목적: 기존 synchronous render runner 앞에 resource lease lifecycle을 결합
// 입력: delegate: 실제 processing runner, admission: local 또는 Router admission, configuration: declared claim
// 출력: processing dependency보다 먼저 파괴되어야 하는 admission-aware runner
AdmissionRenderJobRunner::AdmissionRenderJobRunner(const runtime::IRenderJobRunner& delegate,
                                                   IRenderResourceAdmission& admission,
                                                   ResourceAdmissionConfiguration configuration)
    : m_delegate(delegate), m_admission(admission), m_configuration(std::move(configuration))
{}

// 목적: render 실행 전 lease acquire, 실행 중 renew, 종료 후 release 수행
// 입력: request: resolved processing request, cancellationToken: scheduler cancellation state
// 출력: admission 실패 또는 renew loss면 CoreError failure, 아니면 delegate terminal result
runtime::RenderJobExecutionResult AdmissionRenderJobRunner::execute(
    const core::render::ResolvedRenderRequest& request, const core::types::CancellationToken& cancellationToken) const
{
    if (cancellationToken.isCancellationRequested())
    {
        return runtime::RenderJobExecutionResult::success(makeCancellationFailure());
    }

    ResourceAdmissionDecision decision = m_admission.acquire(makeLeaseRequest(m_configuration));
    if (!decision.granted())
    {
        if (decision.status == ResourceAdmissionStatus::Busy)
        {
            return runtime::RenderJobExecutionResult::failure(makeResourceBusy(decision));
        }
        return runtime::RenderJobExecutionResult::success(makeAdmissionFailure(decision));
    }

    LeaseReleaseGuard releaseGuard(m_admission, std::move(*decision.lease));
    const RenderResourceLease& lease = releaseGuard.lease();
    std::optional<std::string> renewalFailure;
    const runtime::RenderJobExecutionResult result = [&]() {
        if (lease.expiresAt == std::chrono::system_clock::time_point::max())
        {
            return m_delegate.execute(request, cancellationToken);
        }

        LeaseRenewalGuard renewalGuard(m_admission, lease, m_configuration.resourceRouterLeaseTtl, cancellationToken);
        runtime::RenderJobExecutionResult delegateResult = m_delegate.execute(request, renewalGuard.token());
        renewalGuard.stop();
        if (renewalGuard.hasRenewalFailure())
        {
            renewalFailure = renewalGuard.failureDiagnostic();
        }
        return delegateResult;
    }();

    if (renewalFailure.has_value())
    {
        return runtime::RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
            {{core::types::ErrorCode::Unknown, QString::fromStdString(*renewalFailure)}, {}}));
    }
    return result;
}

}  // namespace flexraw::worker::admission

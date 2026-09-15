#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <gtest/gtest.h>

#include "admission_render_job_runner.h"
#include "current_process_memory_probe.h"
#include "current_system_memory_probe.h"
#include "job_scheduler.h"
#include "local_memory_admission.h"
#include "resource_router_admission.h"
#include "system_memory_probe.h"

namespace flexraw::worker::admission
{
namespace
{

constexpr std::uint64_t MiB = MebibyteBytes;

// 목적: Qt event를 처리하며 HTTP fake server condition이 만족될 때까지 bounded wait
// 입력: predicate: 완료 판정, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 predicate가 true가 되면 true
[[nodiscard]] bool waitForCondition(const std::function<bool()>& predicate, const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return predicate();
}

class FakeSystemMemoryProbe final : public platform::ISystemMemoryProbe
{
public:
    // 목적: test가 지정한 deterministic system memory snapshot 반환
    // 입력: 없음
    // 출력: 현재 configured snapshot 또는 unavailable nullopt
    [[nodiscard]] std::optional<platform::SystemMemorySnapshot> snapshot() const override
    {
        return m_snapshot;
    }

    std::optional<platform::SystemMemorySnapshot> m_snapshot;
};

class FakeResourceRouter final
{
public:
    struct Response
    {
        int statusCode{200};
        QByteArray body;
    };

    // 목적: localhost ephemeral port에서 최소 HTTP response fake server 시작
    // 입력: 없음
    // 출력: Resource Router adapter contract test에 사용할 listening server
    FakeResourceRouter()
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0))
        {
            throw std::runtime_error("Unable to listen for Resource Router test server.");
        }
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() { acceptConnections(); });
    }

    // 목적: 다음 HTTP request에 반환할 status/body response enqueue
    // 입력: statusCode: HTTP status, body: optional JSON response body
    // 출력: subsequent request 하나에만 대응하는 response 추가
    void enqueueResponse(const int statusCode, QByteArray body = {})
    {
        m_responses.push_back({statusCode, std::move(body)});
    }

    // 목적: configured fake server의 Resource Router base URL 반환
    // 입력: 없음
    // 출력: HTTP adapter constructor에 전달 가능한 localhost endpoint
    [[nodiscard]] std::string endpoint() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()).toStdString();
    }

    // 목적: 처리 완료된 HTTP request 수 조회
    // 입력: 없음
    // 출력: request line과 body가 모두 수집된 request 개수
    [[nodiscard]] std::size_t requestCount() const noexcept
    {
        return m_requests.size();
    }

    // 목적: 요청 순서 기준으로 수집한 HTTP raw request 반환
    // 입력: index: 처리된 request index
    // 출력: request line/header/body를 포함한 raw bytes
    [[nodiscard]] const QByteArray& requestAt(const std::size_t index) const
    {
        return m_requests.at(index);
    }

private:
    struct ConnectionState
    {
        QTcpSocket* socket{nullptr};
        QByteArray bytes;
        bool responded{false};
    };

    // 목적: pending TCP socket별 request accumulator를 만들고 readyRead handler 연결
    // 입력: 없음
    // 출력: 각 client connection이 complete HTTP request를 받을 준비 완료
    void acceptConnections()
    {
        while (m_server.hasPendingConnections())
        {
            QTcpSocket* const socket = m_server.nextPendingConnection();
            auto state = std::make_unique<ConnectionState>();
            state->socket = socket;
            ConnectionState* const statePointer = state.get();
            QObject::connect(
                socket, &QTcpSocket::readyRead, socket, [this, statePointer]() { processConnection(*statePointer); });
            m_connections.push_back(std::move(state));
        }
    }

    // 목적: HTTP header의 Content-Length를 읽어 complete request byte size 계산
    // 입력: bytes: socket에서 누적한 raw request data, headerEnd: CRLFCRLF 위치
    // 출력: body가 없는/있는 request의 전체 byte 수, header 불완전/invalid면 nullopt
    [[nodiscard]] static std::optional<qsizetype> completeRequestSize(const QByteArray& bytes,
                                                                      const qsizetype headerEnd)
    {
        const QList<QByteArray> headerLines = bytes.left(headerEnd).split('\n');
        qsizetype contentLength = 0;
        for (const QByteArray& rawLine : headerLines)
        {
            const QByteArray line = rawLine.trimmed();
            if (line.startsWith("Content-Length:"))
            {
                bool parsed = false;
                const qlonglong value = line.mid(QByteArray("Content-Length:").size()).trimmed().toLongLong(&parsed);
                if (!parsed || value < 0 || value > std::numeric_limits<qsizetype>::max())
                {
                    return std::nullopt;
                }
                contentLength = static_cast<qsizetype>(value);
            }
        }
        return headerEnd + 4 + contentLength;
    }

    // 목적: 누적 socket data가 complete request가 되면 queued response를 전송
    // 입력: state: one TCP connection의 read buffer와 response flag
    // 출력: raw request 기록 후 HTTP response write 및 connection close
    void processConnection(ConnectionState& state)
    {
        if (state.responded)
        {
            return;
        }
        state.bytes.append(state.socket->readAll());
        const qsizetype headerEnd = state.bytes.indexOf("\r\n\r\n");
        if (headerEnd < 0)
        {
            return;
        }
        const std::optional<qsizetype> completeSize = completeRequestSize(state.bytes, headerEnd);
        if (!completeSize.has_value() || state.bytes.size() < *completeSize)
        {
            return;
        }

        state.responded = true;
        m_requests.push_back(state.bytes.left(*completeSize));
        const Response response = m_responses.empty() ? Response{500, QByteArray("{\"code\":\"UNEXPECTED_REQUEST\"}")}
                                                      : std::exchange(m_responses.front(), Response{});
        if (!m_responses.empty())
        {
            m_responses.pop_front();
        }

        const QByteArray header = "HTTP/1.1 " + QByteArray::number(response.statusCode) +
                                  " Test\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Content-Length: " +
                                  QByteArray::number(response.body.size()) +
                                  "\r\n"
                                  "Connection: close\r\n\r\n";
        state.socket->write(header);
        state.socket->write(response.body);
        state.socket->flush();
        state.socket->disconnectFromHost();
    }

    QTcpServer m_server;
    std::deque<Response> m_responses;
    std::vector<std::unique_ptr<ConnectionState>> m_connections;
    std::vector<QByteArray> m_requests;
};

class ImmediateRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: admission runner test에서 processing 없이 고정 render success 반환
    // 입력: request: output artifact path, cancellationToken: cancellation state
    // 출력: cancellation이면 failure, 아니면 fixed success
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        m_executeCount.fetch_add(1, std::memory_order_relaxed);
        if (cancellationToken.isCancellationRequested())
        {
            return runtime::RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
                {{core::types::ErrorCode::Cancelled, QStringLiteral("cancelled")}, {}}));
        }
        return runtime::RenderJobExecutionResult::success(
            core::render::ResolvedRenderPipelineResult::success({{request.outputPath, 1}, {}}));
    }

    // 목적: delegate render invocation count 조회
    // 입력: 없음
    // 출력: execute 호출 횟수
    [[nodiscard]] int executeCount() const noexcept
    {
        return m_executeCount.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic_int m_executeCount{0};
};

class ThrowingRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: lease acquire 이후 delegate exception 경로를 deterministic하게 생성
    // 입력: request/cancellationToken: 사용하지 않는 runner contract 값
    // 출력: 없음; 항상 runtime_error throw
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        static_cast<void>(request);
        static_cast<void>(cancellationToken);
        throw std::runtime_error("delegate failure");
    }
};

class CancellationObservingRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: renewal failure가 전달한 cancellation token을 기다리는 blocking test runner
    // 입력: request: 사용하지 않는 processing request, cancellationToken: admission runner combined token
    // 출력: cancellation 관찰 시 Cancelled failure, timeout이면 success
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        static_cast<void>(request);
        m_started.release();
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            if (cancellationToken.isCancellationRequested())
            {
                return runtime::RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
                    {{core::types::ErrorCode::Cancelled, QStringLiteral("renewal cancelled")}, {}}));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return runtime::RenderJobExecutionResult::success(
            core::render::ResolvedRenderPipelineResult::success({{QStringLiteral("unexpected.jpg"), 1}, {}}));
    }

    // 목적: delegate가 execute에 진입할 때까지 bounded wait
    // 입력: 없음
    // 출력: timeout 전에 started semaphore를 획득하면 true
    [[nodiscard]] bool waitForStarted() const
    {
        return m_started.tryAcquire(1, 3000);
    }

private:
    mutable QSemaphore m_started;
};

class RecordingResourceAdmission final : public IRenderResourceAdmission
{
public:
    // 목적: test가 지정한 acquire decision을 반환하고 call count 기록
    // 입력: request: 관찰할 resource request
    // 출력: configured acquire decision
    [[nodiscard]] ResourceAdmissionDecision acquire(const ResourceLeaseRequest& request) override
    {
        m_lastRequest = request;
        m_acquireCount.fetch_add(1, std::memory_order_relaxed);
        return m_acquireDecision;
    }

    // 목적: test가 지정한 renewal decision을 반환하고 call count 기록
    // 입력: lease/ttl: 관찰할 active lease lifecycle 값
    // 출력: configured renewal decision
    [[nodiscard]] ResourceAdmissionDecision renew(const RenderResourceLease& lease,
                                                  const std::chrono::seconds ttl) override
    {
        m_lastRenewLease = lease;
        m_lastRenewTtl = ttl;
        m_renewCount.fetch_add(1, std::memory_order_relaxed);
        return m_renewDecision;
    }

    // 목적: test가 지정한 release acknowledgement를 반환하고 call count 기록
    // 입력: lease: 관찰할 terminal lease
    // 출력: configured release result
    [[nodiscard]] ResourceLeaseReleaseResult release(const RenderResourceLease& lease) noexcept override
    {
        m_lastReleasedLease = lease;
        m_releaseCount.fetch_add(1, std::memory_order_relaxed);
        return m_releaseResult;
    }

    ResourceAdmissionDecision m_acquireDecision;
    ResourceAdmissionDecision m_renewDecision;
    ResourceLeaseReleaseResult m_releaseResult{true, {}};
    ResourceLeaseRequest m_lastRequest;
    RenderResourceLease m_lastRenewLease;
    std::chrono::seconds m_lastRenewTtl{0};
    RenderResourceLease m_lastReleasedLease;
    std::atomic_int m_acquireCount{0};
    std::atomic_int m_renewCount{0};
    std::atomic_int m_releaseCount{0};
};

// 목적: Router/local admission test용 valid resource lease request 생성
// 입력: requestId: idempotency identity, memoryMiB: declared memory commitment
// 출력: CPU 1 core를 포함한 v1-compatible lease request
[[nodiscard]] ResourceLeaseRequest makeLeaseRequest(const std::string& requestId, const std::uint64_t memoryMiB = 256)
{
    return {requestId, "flexraw-worker-test", {memoryMiB, 1.0, false}, std::chrono::seconds(60)};
}

// 목적: AdmissionRenderJobRunner test용 minimal resolved request 생성
// 입력: 없음
// 출력: fixed output path와 default develop/export option을 가진 request
[[nodiscard]] core::render::ResolvedRenderRequest makeRenderRequest()
{
    return {QStringLiteral("source.CR3"), QStringLiteral("output.jpg"), {}, {}};
}

TEST(ResourceAdmissionConfigurationTest, UsesMeasuredRenderMemoryClaimByDefault)
{
    const ResourceAdmissionConfiguration configuration;

    EXPECT_EQ(640U, DefaultRenderMemoryClaimMiB);
    EXPECT_EQ(DefaultRenderMemoryClaimMiB, configuration.renderMemoryClaimMiB);
}

TEST(LocalMemoryAdmissionTest, PreservesConfiguredMinimumAvailableReserve)
{
    FakeSystemMemoryProbe probe;
    probe.m_snapshot = platform::SystemMemorySnapshot{8 * 1024 * MiB, 1024 * MiB};
    LocalMemoryAdmission admission(probe, 512);

    const ResourceAdmissionDecision oversized = admission.acquire(makeLeaseRequest("oversized", 513));
    EXPECT_EQ(ResourceAdmissionStatus::Busy, oversized.status);

    const ResourceAdmissionDecision fitting = admission.acquire(makeLeaseRequest("fitting", 512));
    ASSERT_TRUE(fitting.granted());
    EXPECT_TRUE(admission.release(*fitting.lease).released);
}

TEST(LocalMemoryAdmissionTest, KeepsActiveClaimInLocalLedgerUntilReleased)
{
    FakeSystemMemoryProbe probe;
    probe.m_snapshot = platform::SystemMemorySnapshot{1024 * MiB, 1024 * MiB};
    LocalMemoryAdmission admission(probe, 512);

    const ResourceAdmissionDecision first = admission.acquire(makeLeaseRequest("first", 400));
    ASSERT_TRUE(first.granted());
    EXPECT_TRUE(admission.acquire(makeLeaseRequest("second", 200)).status == ResourceAdmissionStatus::Busy);
    EXPECT_TRUE(admission.release(*first.lease).released);
    EXPECT_TRUE(admission.acquire(makeLeaseRequest("second", 200)).granted());
}

TEST(LocalMemoryAdmissionTest, RejectsReusedRequestIdWithDifferentClaim)
{
    FakeSystemMemoryProbe probe;
    probe.m_snapshot = platform::SystemMemorySnapshot{2048 * MiB, 2048 * MiB};
    LocalMemoryAdmission admission(probe, 512);

    const ResourceAdmissionDecision first = admission.acquire(makeLeaseRequest("same-request", 256));
    ASSERT_TRUE(first.granted());
    EXPECT_EQ(ResourceAdmissionStatus::InvalidRequest, admission.acquire(makeLeaseRequest("same-request", 512)).status);
    EXPECT_TRUE(admission.release(*first.lease).released);
}

TEST(LocalMemoryAdmissionTest, ReportsUnavailableWhenPlatformProbeCannotReadMemory)
{
    FakeSystemMemoryProbe probe;
    LocalMemoryAdmission admission(probe, 512);

    EXPECT_EQ(ResourceAdmissionStatus::Unavailable, admission.acquire(makeLeaseRequest("unavailable")).status);
}

TEST(ResourceRouterAdmissionTest, EncodesAcquireAndMapsGrantedLease)
{
    FakeResourceRouter router;
    router.enqueueResponse(
        200,
        QByteArray(
            "{\"status\":\"GRANTED\",\"lease\":{\"lease_id\":\"lease-1\",\"expires_at\":\"2026-08-21T00:01:00Z\"}}"));
    ResourceRouterAdmission admission(router.endpoint(), std::chrono::milliseconds(1000));

    const ResourceAdmissionDecision decision =
        admission.acquire(makeLeaseRequest("018f5c30-0000-0000-0000-000000000001", 768));

    ASSERT_TRUE(decision.granted());
    EXPECT_EQ("lease-1", decision.lease->id);
    ASSERT_TRUE(waitForCondition([&]() { return router.requestCount() == 1; }));
    const QByteArray request = router.requestAt(0);
    EXPECT_TRUE(request.startsWith("POST /v1/leases HTTP/1.1"));
    EXPECT_TRUE(request.contains("\"request_id\":\"018f5c30-0000-0000-0000-000000000001\""));
    EXPECT_TRUE(request.contains("\"memory_mb\":768"));
    EXPECT_TRUE(request.contains("\"gpu\":false"));
    EXPECT_TRUE(request.contains("\"cpu_cores\":1"));
}

TEST(ResourceRouterAdmissionTest, MapsBusyAndAdvisoryRetryWithoutLease)
{
    FakeResourceRouter router;
    router.enqueueResponse(
        409, QByteArray("{\"status\":\"BUSY\",\"reason\":\"INSUFFICIENT_MEMORY\",\"retry_after_ms\":3000}"));
    ResourceRouterAdmission admission(router.endpoint(), std::chrono::milliseconds(1000));

    const ResourceAdmissionDecision decision = admission.acquire(makeLeaseRequest("busy-request"));

    EXPECT_EQ(ResourceAdmissionStatus::Busy, decision.status);
    ASSERT_TRUE(decision.retryAfter.has_value());
    EXPECT_EQ(std::chrono::milliseconds(3000), *decision.retryAfter);
    EXPECT_EQ("INSUFFICIENT_MEMORY", decision.diagnostic);
}

TEST(ResourceRouterAdmissionTest, PreservesUnavailableRetryAdviceWithoutReissuingKnownSafeFailure)
{
    FakeResourceRouter router;
    router.enqueueResponse(
        503, QByteArray("{\"code\":\"ROUTER_RECOVERING\",\"message\":\"reconciling\",\"retry_after_ms\":2500}"));
    ResourceRouterAdmission admission(router.endpoint(), std::chrono::milliseconds(1000));

    const ResourceAdmissionDecision decision = admission.acquire(makeLeaseRequest("recovering-request"));

    EXPECT_EQ(ResourceAdmissionStatus::Unavailable, decision.status);
    ASSERT_TRUE(decision.retryAfter.has_value());
    EXPECT_EQ(std::chrono::milliseconds(2500), *decision.retryAfter);
    ASSERT_TRUE(waitForCondition([&]() { return router.requestCount() == 1; }));
}

TEST(ResourceRouterAdmissionTest, ResolvesUnknownStorageOutcomeWithSameAcquireRequest)
{
    FakeResourceRouter router;
    router.enqueueResponse(503,
                           QByteArray("{\"code\":\"STORAGE_UNAVAILABLE\",\"message\":\"commit outcome unknown\"}"));
    router.enqueueResponse(200,
                           QByteArray("{\"status\":\"GRANTED\",\"lease\":{\"lease_id\":\"lease-resolved\",\"expires_"
                                      "at\":\"2026-08-21T00:01:00Z\"}}"));
    ResourceRouterAdmission admission(router.endpoint(), std::chrono::milliseconds(1000));

    const ResourceAdmissionDecision decision = admission.acquire(makeLeaseRequest("stable-request", 768));

    ASSERT_TRUE(decision.granted());
    EXPECT_EQ("lease-resolved", decision.lease->id);
    ASSERT_TRUE(waitForCondition([&]() { return router.requestCount() == 2; }));
    EXPECT_TRUE(router.requestAt(0).contains("\"request_id\":\"stable-request\""));
    EXPECT_TRUE(router.requestAt(1).contains("\"request_id\":\"stable-request\""));
    EXPECT_TRUE(router.requestAt(0).contains("\"memory_mb\":768"));
    EXPECT_TRUE(router.requestAt(1).contains("\"memory_mb\":768"));
}

TEST(ResourceRouterAdmissionTest, RenewsThenReleasesUsingV1LeasePaths)
{
    FakeResourceRouter router;
    router.enqueueResponse(
        200, QByteArray("{\"lease_id\":\"lease-2\",\"status\":\"ACTIVE\",\"expires_at\":\"2026-08-21T00:02:00Z\"}"));
    router.enqueueResponse(204);
    ResourceRouterAdmission admission(router.endpoint(), std::chrono::milliseconds(1000));
    const RenderResourceLease lease{"lease-2", std::chrono::system_clock::now()};

    const ResourceAdmissionDecision renewed = admission.renew(lease, std::chrono::seconds(60));
    ASSERT_TRUE(renewed.granted());
    EXPECT_EQ("lease-2", renewed.lease->id);
    EXPECT_TRUE(admission.release(*renewed.lease).released);

    ASSERT_TRUE(waitForCondition([&]() { return router.requestCount() == 2; }));
    EXPECT_TRUE(router.requestAt(0).startsWith("POST /v1/leases/lease-2/renew HTTP/1.1"));
    EXPECT_TRUE(router.requestAt(0).contains("\"ttl_seconds\":60"));
    EXPECT_TRUE(router.requestAt(1).startsWith("DELETE /v1/leases/lease-2 HTTP/1.1"));
}

TEST(AdmissionRenderJobRunnerTest, PreservesBusyAndRetryAdviceBeforeDelegateExecution)
{
    ImmediateRenderJobRunner delegate;
    RecordingResourceAdmission admission;
    admission.m_acquireDecision = {
        ResourceAdmissionStatus::Busy, std::nullopt, std::chrono::milliseconds(3000), "INSUFFICIENT_MEMORY"};
    AdmissionRenderJobRunner runner(delegate, admission, {});
    core::types::CancellationSource cancellation;

    const runtime::RenderJobExecutionResult result = runner.execute(makeRenderRequest(), cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(QStringLiteral("INSUFFICIENT_MEMORY"), result.error().message);
    ASSERT_TRUE(result.error().retryAfter.has_value());
    EXPECT_EQ(std::chrono::milliseconds(3000), *result.error().retryAfter);
    EXPECT_EQ(0, delegate.executeCount());
    EXPECT_EQ(1, admission.m_acquireCount.load(std::memory_order_relaxed));
}

TEST(AdmissionRenderJobRunnerTest, CancelsPipelineAndFailsWhenLeaseRenewalIsLost)
{
    CancellationObservingRenderJobRunner delegate;
    RecordingResourceAdmission admission;
    admission.m_acquireDecision = {
        ResourceAdmissionStatus::Granted,
        RenderResourceLease{"lease-3", std::chrono::system_clock::now() + std::chrono::milliseconds(1)},
        std::nullopt,
        {}};
    admission.m_renewDecision = {ResourceAdmissionStatus::Unavailable, std::nullopt, std::nullopt, "ROUTER_RECOVERING"};
    ResourceAdmissionConfiguration configuration;
    configuration.resourceRouterLeaseTtl = std::chrono::seconds(60);
    AdmissionRenderJobRunner runner(delegate, admission, configuration);
    core::types::CancellationSource cancellation;

    const runtime::RenderJobExecutionResult result = runner.execute(makeRenderRequest(), cancellation.token());

    ASSERT_TRUE(result.hasValue());
    ASSERT_TRUE(result.value().hasError());
    EXPECT_EQ(core::types::ErrorCode::Unknown, result.value().error().cause.code);
    EXPECT_EQ(QStringLiteral("ROUTER_RECOVERING"), result.value().error().cause.message);
    EXPECT_EQ(1, admission.m_renewCount.load(std::memory_order_relaxed));
    EXPECT_EQ(1, admission.m_releaseCount.load(std::memory_order_relaxed));
}

TEST(AdmissionRenderJobRunnerTest, ReleasesLeaseWhenDelegateThrows)
{
    ThrowingRenderJobRunner delegate;
    RecordingResourceAdmission admission;
    admission.m_acquireDecision = {ResourceAdmissionStatus::Granted,
                                   RenderResourceLease{"local-lease", std::chrono::system_clock::time_point::max()},
                                   std::nullopt,
                                   {}};
    AdmissionRenderJobRunner runner(delegate, admission, {});
    core::types::CancellationSource cancellation;

    EXPECT_THROW(static_cast<void>(runner.execute(makeRenderRequest(), cancellation.token())), std::runtime_error);
    EXPECT_EQ(1, admission.m_releaseCount.load(std::memory_order_relaxed));
}

TEST(ResourceAdmissionIntegrationTest, UsesRealRouterAdapterFromSchedulerWorkerThread)
{
    FakeResourceRouter router;
    router.enqueueResponse(200,
                           QByteArray("{\"status\":\"GRANTED\",\"lease\":{\"lease_id\":\"thread-lease\",\"expires_at\":"
                                      "\"2099-08-21T00:01:00Z\"}}"));
    router.enqueueResponse(204);
    ResourceRouterAdmission admission(router.endpoint(), std::chrono::milliseconds(1000));
    ImmediateRenderJobRunner delegate;
    ResourceAdmissionConfiguration configuration;
    configuration.renderMemoryClaimMiB = 512;
    AdmissionRenderJobRunner runner(delegate, admission, configuration);
    runtime::JobScheduler scheduler(runner, 1, 0);
    std::mutex outcomeMutex;
    std::optional<runtime::RenderJobOutcome> outcome;

    const runtime::SubmitStatus status = scheduler.submit({{1, {1}}, QStringLiteral("output.jpg"), makeRenderRequest()},
                                                          [&](runtime::RenderJobOutcome completed) {
                                                              const std::scoped_lock lock(outcomeMutex);
                                                              outcome = std::move(completed);
                                                          });

    ASSERT_EQ(runtime::SubmitStatus::Accepted, status);
    ASSERT_TRUE(waitForCondition([&]() {
        const std::scoped_lock lock(outcomeMutex);
        return outcome.has_value();
    }));
    scheduler.shutdown();
    ASSERT_TRUE(waitForCondition([&]() { return router.requestCount() == 2; }));
    const std::scoped_lock lock(outcomeMutex);
    ASSERT_TRUE(outcome->result.hasValue());
    ASSERT_TRUE(outcome->result.value().hasValue());
    EXPECT_EQ(1, delegate.executeCount());
    EXPECT_TRUE(router.requestAt(0).startsWith("POST /v1/leases HTTP/1.1"));
    EXPECT_TRUE(router.requestAt(1).startsWith("DELETE /v1/leases/thread-lease HTTP/1.1"));
}

TEST(PlatformMemoryProbeTest, CurrentPlatformAdapterReportsPlausiblePhysicalMemorySnapshot)
{
    const std::unique_ptr<platform::ISystemMemoryProbe> probe = platform::createCurrentSystemMemoryProbe();
    ASSERT_NE(nullptr, probe);
    const std::optional<platform::SystemMemorySnapshot> snapshot = probe->snapshot();

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_GT(snapshot->totalBytes, 0U);
    EXPECT_GT(snapshot->availableBytes, 0U);
    EXPECT_LE(snapshot->availableBytes, snapshot->totalBytes);
}

TEST(PlatformMemoryProbeTest, CurrentPlatformAdapterReportsPlausibleProcessMemorySnapshot)
{
    const std::unique_ptr<platform::IProcessMemoryProbe> probe = platform::createCurrentProcessMemoryProbe();
    ASSERT_NE(nullptr, probe);
    const std::optional<platform::ProcessMemorySnapshot> snapshot = probe->snapshot();

    ASSERT_TRUE(snapshot.has_value());
    EXPECT_GT(snapshot->residentBytes, 0U);
    EXPECT_GE(snapshot->peakResidentBytes, snapshot->residentBytes);
}

}  // namespace
}  // namespace flexraw::worker::admission

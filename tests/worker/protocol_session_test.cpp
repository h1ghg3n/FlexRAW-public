#include <chrono>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QSemaphore>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>

#include <gtest/gtest.h>

#include "frame_codec.h"
#include "frame_parser.h"
#include "health_payload_codec.h"
#include "job_scheduler.h"
#include "render_job_runner.h"
#include "render_payload_codec.h"
#include "render_worker_runtime_facade.h"
#include "worker_path_resolver.h"
#include "worker_server.h"

namespace flexraw::worker::network
{
namespace
{

// 목적: Qt event를 처리하며 predicate가 참이 될 때까지 bounded wait
// 입력: predicate: 완료 조건, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 조건을 만족하면 true
[[nodiscard]] bool waitUntil(const std::function<bool()>& predicate, const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

// 목적: path resolver test root에 최소 source file 생성
// 입력: path: 생성할 file path
// 출력: 생성 성공 여부
[[nodiscard]] bool createSourceFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("raw") == 3;
}

// 목적: temporary source/output root에서 WorkerPathResolver 생성
// 입력: sourceRoot/outputRoot: 기존 temporary directory
// 출력: 검증된 resolver 또는 test setup 예외
[[nodiscard]] runtime::WorkerPathResolver createResolver(const QTemporaryDir& sourceRoot,
                                                         const QTemporaryDir& outputRoot)
{
    runtime::WorkerPathResolver::CreateResult result =
        runtime::WorkerPathResolver::create({sourceRoot.path(), outputRoot.path()});
    if (result.hasError())
    {
        throw std::runtime_error(result.error().message.toStdString());
    }
    return result.value();
}

class ImmediateRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: protocol success path를 실제 image 처리 없이 즉시 완료
    // 입력: request: artifact output path, cancellationToken: 취소 시 failure 선택
    // 출력: 고정 byte size/stats의 success 또는 Cancelled failure
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        if (cancellationToken.isCancellationRequested())
        {
            return runtime::RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
                {{core::types::ErrorCode::Cancelled, QStringLiteral("cancelled")}, {}}));
        }
        return runtime::RenderJobExecutionResult::success(
            core::render::ResolvedRenderPipelineResult::success({{request.outputPath, 42}, {1, 2, 3, 4, 5}}));
    }
};

class ResourceBusyRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: accepted job 이후 structured resource busy terminal path 생성
    // 입력: request/cancellationToken: 사용하지 않는 runner contract 값
    // 출력: fixed retry advice가 있는 ResourceBusy
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        static_cast<void>(request);
        static_cast<void>(cancellationToken);
        return runtime::RenderJobExecutionResult::failure(
            {QStringLiteral("INSUFFICIENT_MEMORY"), std::chrono::milliseconds(3000)});
    }
};

class BlockingRenderJobRunner final : public runtime::IRenderJobRunner
{
public:
    // 목적: test별 cooperative cancellation 준수 여부 선택
    // 입력: honorCancellation: true면 cancellation 요청 시 즉시 종료
    // 출력: release 가능한 blocking runner
    explicit BlockingRenderJobRunner(const bool honorCancellation = true) : m_honorCancellation(honorCancellation) {}

    // 목적: queue/cancel/session-close test가 제어할 때까지 render job block
    // 입력: request: 성공 artifact path, cancellationToken: cooperative cancellation state
    // 출력: release 시 success, cancellation 시 Cancelled failure
    [[nodiscard]] runtime::RenderJobExecutionResult execute(
        const core::render::ResolvedRenderRequest& request,
        const core::types::CancellationToken& cancellationToken) const override
    {
        m_started.release();
        while (!m_release.tryAcquire(1, 5))
        {
            if (m_honorCancellation && cancellationToken.isCancellationRequested())
            {
                return runtime::RenderJobExecutionResult::success(core::render::ResolvedRenderPipelineResult::failure(
                    {{core::types::ErrorCode::Cancelled, QStringLiteral("cancelled")}, {}}));
            }
        }
        return runtime::RenderJobExecutionResult::success(
            core::render::ResolvedRenderPipelineResult::success({{request.outputPath, 42}, {}}));
    }

    // 목적: runner가 job 실행을 시작할 때까지 bounded wait
    // 입력: count: 기다릴 job 수
    // 출력: timeout 전에 모두 시작했으면 true
    [[nodiscard]] bool waitForStarted(const int count = 1) const
    {
        bool started = false;
        static_cast<void>(waitUntil([this, count, &started]() {
            started = started || m_started.tryAcquire(count);
            return started;
        }));
        return started;
    }

    // 목적: block된 job을 success 경로로 진행
    // 입력: count: release할 invocation 수
    // 출력: 없음
    void release(const int count = 1) const
    {
        m_release.release(count);
    }

private:
    bool m_honorCancellation;
    mutable QSemaphore m_started;
    mutable QSemaphore m_release;
};

class CompletionWinningRuntime final : public runtime::IRenderWorkerRuntime
{
public:
    // 목적: submit 반환 전에 Runtime terminal을 발생시켜 session delivery race를 결정적으로 구성
    // 입력: command: Runtime identity와 output intent, completion: ProtocolSession callback
    // 출력: completion을 먼저 호출한 뒤 Accepted
    [[nodiscard]] runtime::RenderWorkerSubmitResult submit(runtime::RenderWorkerCommand command,
                                                           runtime::RenderJobCompletion completion) override
    {
        ++m_submitCount;
        const runtime::RenderJobExecutionResult result = runtime::RenderJobExecutionResult::success(
            core::render::ResolvedRenderPipelineResult::success({{command.request.outputRelativePath, 42}, {}}));
        completion({command.key, command.request.outputRelativePath, result});
        return runtime::RenderWorkerSubmitResult::success(runtime::SubmitStatus::Accepted);
    }

    // 목적: completion이 이미 Runtime authority를 제거한 late cancel 모사
    // 입력: key: session-scoped job identity
    // 출력: active job이 없으므로 false
    [[nodiscard]] bool cancel(const runtime::RenderJobKey key) override
    {
        static_cast<void>(key);
        ++m_cancelCount;
        return false;
    }

    // 목적: protocol health contract를 만족하는 최소 ready snapshot 제공
    // 입력: 없음
    // 출력: accepting 상태의 빈 Runtime snapshot
    [[nodiscard]] runtime::WorkerRuntimeSnapshot snapshot() const override
    {
        return {0, 0, 1, 1, {}, true};
    }

    // 목적: submit 호출 횟수 조회
    // 입력: 없음
    // 출력: accepted request 수
    [[nodiscard]] int submitCount() const noexcept
    {
        return m_submitCount;
    }

    // 목적: completion-won 구간에 도착한 cancel 호출 횟수 조회
    // 입력: 없음
    // 출력: Runtime cancel 호출 수
    [[nodiscard]] int cancelCount() const noexcept
    {
        return m_cancelCount;
    }

private:
    int m_submitCount{0};
    int m_cancelCount{0};
};

class RuntimeServerHarness final
{
public:
    // 목적: injected Runtime port를 ephemeral localhost WorkerServer에 연결
    // 입력: runtime: protocol race를 제어할 fake Runtime
    // 출력: 즉시 listen하는 server harness
    explicit RuntimeServerHarness(runtime::IRenderWorkerRuntime& runtime) : m_server(runtime)
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0))
        {
            throw std::runtime_error("Unable to create Runtime WorkerServer test harness.");
        }
    }

    // 목적: client 연결용 ephemeral port 조회
    // 입력: 없음
    // 출력: bound server port
    [[nodiscard]] quint16 port() const noexcept
    {
        return m_server.serverPort();
    }

private:
    WorkerServer m_server;
};

class WorkerServerHarness final
{
public:
    // 목적: temporary roots, bounded scheduler와 localhost WorkerServer 구성
    // 입력: runner: test 실행자, concurrency/queueCapacity/serverConfiguration: runtime 한도
    // 출력: ephemeral port에서 listen하는 server harness
    WorkerServerHarness(const runtime::IRenderJobRunner& runner,
                        const std::uint32_t concurrency = 1,
                        const std::uint64_t queueCapacity = 1,
                        WorkerServerConfiguration serverConfiguration = {})
        : m_resolver(createResolver(m_sourceRoot, m_outputRoot)),
          m_scheduler(runner, concurrency, queueCapacity),
          m_runtimePort(m_resolver, m_scheduler),
          m_server(m_runtimePort, std::move(serverConfiguration))
    {
        if (!m_sourceRoot.isValid() || !m_outputRoot.isValid() ||
            !createSourceFile(QDir(m_sourceRoot.path()).filePath(QStringLiteral("input.CR3"))) ||
            !m_server.listen(QHostAddress::LocalHost, 0))
        {
            throw std::runtime_error("Unable to create WorkerServer test harness.");
        }
    }

    // 목적: client가 연결할 ephemeral localhost port 조회
    // 입력: 없음
    // 출력: bound server port
    [[nodiscard]] quint16 port() const noexcept
    {
        return m_server.serverPort();
    }

    // 목적: harness server의 active session 수 조회
    // 입력: 없음
    // 출력: 현재 ProtocolSession 수
    [[nodiscard]] qsizetype sessionCount() const noexcept
    {
        return m_server.sessionCount();
    }

    // 목적: late-completion test에서 scheduler terminal 상태 관찰
    // 입력: 없음
    // 출력: 현재 queue/running 수와 high-water snapshot
    [[nodiscard]] runtime::WorkerRuntimeSnapshot runtimeSnapshot() const
    {
        return m_scheduler.snapshot();
    }

private:
    QTemporaryDir m_sourceRoot;
    QTemporaryDir m_outputRoot;
    runtime::WorkerPathResolver m_resolver;
    runtime::JobScheduler m_scheduler;
    runtime::RenderWorkerRuntimeFacade m_runtimePort;
    WorkerServer m_server;
};

class ProtocolTestClient final
{
public:
    // 목적: localhost WorkerServer에 연결하고 server accept까지 대기
    // 입력: port: ephemeral Worker port
    // 출력: client와 server가 ConnectedState이면 true
    [[nodiscard]] bool connectToServer(const quint16 port)
    {
        m_socket.connectToHost(QHostAddress::LocalHost, port);
        return waitUntil([this]() { return m_socket.state() == QAbstractSocket::ConnectedState; });
    }

    // 목적: protocol frame을 encode해 socket write queue에 제출
    // 입력: frame: client request/cancel frame
    // 출력: encode/write 성공 여부
    [[nodiscard]] bool sendFrame(const protocol::ProtocolFrame& frame)
    {
        const protocol::EncodeFrameResult encoded = protocol::encodeFrame(frame);
        return encoded.hasValue() && m_socket.write(encoded.value()) == encoded.value().size();
    }

    // 목적: 이미 encode된 frame stream을 한 write로 제출
    // 입력: bytes: coalesced 또는 invalid wire stream
    // 출력: 전체 byte가 write queue에 들어갔으면 true
    [[nodiscard]] bool sendBytes(const QByteArray& bytes)
    {
        return m_socket.write(bytes) == bytes.size();
    }

    // 목적: 지정 수의 response frame이 parser에서 완성될 때까지 event loop 처리
    // 입력: count: 필요한 frame 수
    // 출력: timeout 전에 frame 수를 확보하면 true
    [[nodiscard]] bool waitForFrames(const std::size_t count)
    {
        return waitUntil([this, count]() {
            readAvailable();
            return m_frames.size() >= count;
        });
    }

    // 목적: 누적 response frame을 wire 순서대로 반환하고 내부 목록 비우기
    // 입력: 없음
    // 출력: 지금까지 수신한 frame 목록
    [[nodiscard]] std::vector<protocol::ProtocolFrame> takeFrames()
    {
        readAvailable();
        return std::exchange(m_frames, {});
    }

    // 목적: peer가 connection을 종료할 때까지 event loop 처리
    // 입력: 없음
    // 출력: timeout 전에 UnconnectedState이면 true
    [[nodiscard]] bool waitForDisconnected()
    {
        return waitUntil([this]() { return m_socket.state() == QAbstractSocket::UnconnectedState; });
    }

private:
    // 목적: 현재 socket receive buffer를 incremental frame parser에 공급
    // 입력: 없음
    // 출력: 완성 frame을 내부 목록에 누적
    void readAvailable()
    {
        const QByteArray bytes = m_socket.readAll();
        if (bytes.isEmpty())
        {
            return;
        }
        protocol::ParseFramesOutcome outcome = m_parser.append(bytes);
        m_frames.insert(m_frames.end(),
                        std::make_move_iterator(outcome.frames.begin()),
                        std::make_move_iterator(outcome.frames.end()));
    }

    QTcpSocket m_socket;
    protocol::FrameParser m_parser;
    std::vector<protocol::ProtocolFrame> m_frames;
};

// 목적: default processing 값과 root-relative path를 가진 RenderRequest frame 생성
// 입력: jobId: session correlation identity
// 출력: payload encode가 성공한 request frame
[[nodiscard]] protocol::ProtocolFrame makeRenderRequestFrame(const protocol::JobId jobId)
{
    runtime::RenderRequestPayload payload;
    payload.sourceRelativePath = QStringLiteral("input.CR3");
    payload.outputRelativePath = QStringLiteral("output.jpg");
    const runtime::EncodePayloadResult encoded = runtime::encodeRenderRequestPayload(payload);
    EXPECT_TRUE(encoded.hasValue());
    return {protocol::MessageType::RenderRequest, jobId, encoded.hasValue() ? encoded.value() : QByteArray{}};
}

TEST(ProtocolSessionTest, ReturnsHealthSnapshotWithoutClaimingRenderJobIdentity)
{
    ImmediateRenderJobRunner runner;
    WorkerServerHarness harness(runner, 3, 5);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    ASSERT_TRUE(client.sendFrame({protocol::MessageType::HealthRequest, 7, {}}));

    ASSERT_TRUE(client.waitForFrames(1));
    const std::vector<protocol::ProtocolFrame> healthFrames = client.takeFrames();
    ASSERT_EQ(1U, healthFrames.size());
    EXPECT_EQ(protocol::MessageType::HealthResponse, healthFrames.front().messageType);
    EXPECT_EQ(7U, healthFrames.front().jobId);
    const protocol::DecodeHealthPayloadResult health =
        protocol::decodeHealthResponsePayload(healthFrames.front().payload);
    ASSERT_TRUE(health.hasValue());
    EXPECT_EQ(protocol::HealthServiceState::Ready, health.value().serviceState);
    EXPECT_EQ(0U, health.value().runningJobs);
    EXPECT_EQ(0U, health.value().queuedJobs);
    EXPECT_EQ(3U, health.value().maximumConcurrentJobs);
    EXPECT_EQ(5U, health.value().queueCapacity);

    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(7)));
    ASSERT_TRUE(client.waitForFrames(2));
    const std::vector<protocol::ProtocolFrame> renderFrames = client.takeFrames();
    ASSERT_EQ(2U, renderFrames.size());
    EXPECT_EQ(protocol::MessageType::JobAccepted, renderFrames.front().messageType);
    EXPECT_EQ(protocol::MessageType::RenderSucceeded, renderFrames.back().messageType);
}

TEST(ProtocolSessionTest, ReturnsAcceptedThenSucceededForValidRenderRequest)
{
    ImmediateRenderJobRunner runner;
    WorkerServerHarness harness(runner);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(7)));

    ASSERT_TRUE(client.waitForFrames(2));
    const std::vector<protocol::ProtocolFrame> frames = client.takeFrames();
    ASSERT_EQ(2U, frames.size());
    EXPECT_EQ(protocol::MessageType::JobAccepted, frames[0].messageType);
    EXPECT_EQ(protocol::MessageType::RenderSucceeded, frames[1].messageType);
    EXPECT_EQ(7U, frames[0].jobId);
    EXPECT_EQ(7U, frames[1].jobId);
    const runtime::DecodeRenderSucceededResult result = runtime::decodeRenderSucceededPayload(frames[1].payload);
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QStringLiteral("output.jpg"), result.value().outputRelativePath);
    EXPECT_EQ(42U, result.value().byteSize);
    EXPECT_EQ(5U, result.value().stats.totalNanoseconds);
}

TEST(ProtocolSessionTest, PreservesQueuedTerminalWhenCompletionWinsLateCancel)
{
    CompletionWinningRuntime runtime;
    RuntimeServerHarness harness(runtime);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    const protocol::EncodeFrameResult request = protocol::encodeFrame(makeRenderRequestFrame(7));
    const protocol::EncodeFrameResult cancel = protocol::encodeFrame({protocol::MessageType::CancelRequest, 7, {}});
    ASSERT_TRUE(request.hasValue());
    ASSERT_TRUE(cancel.hasValue());

    ASSERT_TRUE(client.sendBytes(request.value() + cancel.value()));
    ASSERT_TRUE(client.waitForFrames(2));
    const std::vector<protocol::ProtocolFrame> firstFrames = client.takeFrames();
    ASSERT_EQ(2U, firstFrames.size());
    EXPECT_EQ(protocol::MessageType::JobAccepted, firstFrames[0].messageType);
    EXPECT_EQ(protocol::MessageType::RenderSucceeded, firstFrames[1].messageType);
    EXPECT_EQ(1, runtime.cancelCount());

    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(8)));
    ASSERT_TRUE(client.waitForFrames(2));
    const std::vector<protocol::ProtocolFrame> secondFrames = client.takeFrames();
    ASSERT_EQ(2U, secondFrames.size());
    EXPECT_EQ(protocol::MessageType::JobAccepted, secondFrames[0].messageType);
    EXPECT_EQ(protocol::MessageType::RenderSucceeded, secondFrames[1].messageType);
    EXPECT_EQ(2, runtime.submitCount());
}

TEST(ProtocolSessionTest, ReturnsAcceptedThenResourceBusyWithRetryAdvice)
{
    ResourceBusyRenderJobRunner runner;
    WorkerServerHarness harness(runner);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(9)));

    ASSERT_TRUE(client.waitForFrames(2));
    const std::vector<protocol::ProtocolFrame> frames = client.takeFrames();
    ASSERT_EQ(2U, frames.size());
    EXPECT_EQ(protocol::MessageType::JobAccepted, frames[0].messageType);
    EXPECT_EQ(protocol::MessageType::ResourceBusy, frames[1].messageType);
    const runtime::DecodeResourceBusyResult busy = runtime::decodeResourceBusyPayload(frames[1].payload);
    ASSERT_TRUE(busy.hasValue());
    EXPECT_EQ(QStringLiteral("INSUFFICIENT_MEMORY"), busy.value().message);
    ASSERT_TRUE(busy.value().retryAfter.has_value());
    EXPECT_EQ(std::chrono::milliseconds(3000), *busy.value().retryAfter);
}

TEST(ProtocolSessionTest, ReturnsJobFailureForMalformedRenderPayload)
{
    ImmediateRenderJobRunner runner;
    WorkerServerHarness harness(runner);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    ASSERT_TRUE(client.sendFrame({protocol::MessageType::RenderRequest, 8, QByteArray("bad")}));

    ASSERT_TRUE(client.waitForFrames(1));
    const std::vector<protocol::ProtocolFrame> frames = client.takeFrames();
    ASSERT_EQ(1U, frames.size());
    EXPECT_EQ(protocol::MessageType::RenderFailed, frames[0].messageType);
    const runtime::DecodeRenderFailedResult failure = runtime::decodeRenderFailedPayload(frames[0].payload);
    ASSERT_TRUE(failure.hasValue());
    EXPECT_EQ(core::types::ErrorCode::InvalidArgument, failure.value().cause.code);
}

TEST(ProtocolSessionTest, ReturnsServerBusyWhenWaitingQueueIsFull)
{
    BlockingRenderJobRunner runner;
    WorkerServerHarness harness(runner, 1, 0);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(1)));
    ASSERT_TRUE(runner.waitForStarted());
    ASSERT_TRUE(client.waitForFrames(1));
    EXPECT_EQ(protocol::MessageType::JobAccepted, client.takeFrames().front().messageType);

    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(2)));
    ASSERT_TRUE(client.waitForFrames(1));
    const std::vector<protocol::ProtocolFrame> busyFrames = client.takeFrames();
    ASSERT_EQ(1U, busyFrames.size());
    EXPECT_EQ(protocol::MessageType::ServerBusy, busyFrames.front().messageType);
    EXPECT_EQ(2U, busyFrames.front().jobId);

    ASSERT_TRUE(client.sendFrame({protocol::MessageType::CancelRequest, 1, {}}));
    ASSERT_TRUE(client.waitForFrames(1));
    EXPECT_EQ(protocol::MessageType::RenderFailed, client.takeFrames().front().messageType);
}

TEST(ProtocolSessionTest, DuplicateActiveJobIdClosesAndCancelsSession)
{
    BlockingRenderJobRunner runner;
    WorkerServerHarness harness(runner, 1, 1);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(3)));
    ASSERT_TRUE(runner.waitForStarted());
    ASSERT_TRUE(client.waitForFrames(1));
    static_cast<void>(client.takeFrames());

    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(3)));

    EXPECT_TRUE(client.waitForDisconnected());
}

TEST(ProtocolSessionTest, IgnoresLateOutcomeAfterSessionDestruction)
{
    BlockingRenderJobRunner runner(false);
    WorkerServerHarness harness(runner, 1, 0);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(4)));
    ASSERT_TRUE(runner.waitForStarted());
    ASSERT_TRUE(client.waitForFrames(1));
    static_cast<void>(client.takeFrames());

    ASSERT_TRUE(client.sendFrame(makeRenderRequestFrame(4)));
    ASSERT_TRUE(client.waitForDisconnected());
    ASSERT_TRUE(waitUntil([&harness]() { return harness.sessionCount() == 0; }));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    runner.release();
    ASSERT_TRUE(waitUntil([&harness]() { return harness.runtimeSnapshot().running == 0; }));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    EXPECT_EQ(0, harness.sessionCount());
}

TEST(ProtocolSessionTest, SameWireJobIdIsIndependentAcrossConnections)
{
    ImmediateRenderJobRunner runner;
    WorkerServerHarness harness(runner, 2, 0);
    ProtocolTestClient first;
    ProtocolTestClient second;
    ASSERT_TRUE(first.connectToServer(harness.port()));
    ASSERT_TRUE(second.connectToServer(harness.port()));
    ASSERT_TRUE(first.sendFrame(makeRenderRequestFrame(1)));
    ASSERT_TRUE(second.sendFrame(makeRenderRequestFrame(1)));

    ASSERT_TRUE(first.waitForFrames(2));
    ASSERT_TRUE(second.waitForFrames(2));
    EXPECT_EQ(protocol::MessageType::RenderSucceeded, first.takeFrames().back().messageType);
    EXPECT_EQ(protocol::MessageType::RenderSucceeded, second.takeFrames().back().messageType);
}

TEST(ProtocolSessionTest, ProcessesValidFrameBeforeInvalidHeaderThenDisconnects)
{
    BlockingRenderJobRunner runner;
    WorkerServerHarness harness(runner, 1, 0);
    ProtocolTestClient client;
    ASSERT_TRUE(client.connectToServer(harness.port()));
    const protocol::EncodeFrameResult valid = protocol::encodeFrame(makeRenderRequestFrame(9));
    ASSERT_TRUE(valid.hasValue());
    QByteArray invalidHeader = valid.value().first(protocol::ProtocolHeaderBytes);
    invalidHeader[0] = 'X';

    ASSERT_TRUE(client.sendBytes(valid.value() + invalidHeader));

    ASSERT_TRUE(client.waitForFrames(1));
    EXPECT_EQ(protocol::MessageType::JobAccepted, client.takeFrames().front().messageType);
    EXPECT_TRUE(client.waitForDisconnected());
}

}  // namespace
}  // namespace flexraw::worker::network

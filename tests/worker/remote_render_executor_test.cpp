#include <chrono>
#include <atomic>
#include <functional>
#include <future>
#include <stdexcept>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <gtest/gtest.h>

#include "frame_codec.h"
#include "frame_parser.h"
#include "remote_render_executor.h"
#include "render_payload_codec.h"

namespace flexraw::worker::client
{
namespace
{

using namespace std::chrono_literals;
using FrameHandler = std::function<void(QTcpSocket&, const protocol::ProtocolFrame&)>;

// 목적: Qt event를 처리하며 asynchronous client future 완료 대기
// 입력: future: remote execute 결과, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 future가 ready이면 true
[[nodiscard]] bool waitForFuture(std::future<RemoteRenderResult>& future, const int timeoutMilliseconds = 3000)
{
    QElapsedTimer timer;
    timer.start();
    while (future.wait_for(0ms) != std::future_status::ready && timer.elapsed() < timeoutMilliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return future.wait_for(0ms) == std::future_status::ready;
}

// 목적: predicate가 참이 될 때까지 Qt server event 처리
// 입력: predicate: 관찰할 server state, timeoutMilliseconds: 최대 대기 시간
// 출력: timeout 전에 predicate가 참이면 true
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

// 목적: response frame 목록을 한 socket write로 coalescing해 전송
// 입력: socket: 연결된 client socket, frames: wire 순서 response 목록
// 출력: 모든 frame encode/write 성공 여부
[[nodiscard]] bool sendFrames(QTcpSocket& socket, const std::vector<protocol::ProtocolFrame>& frames)
{
    QByteArray bytes;
    for (const protocol::ProtocolFrame& frame : frames)
    {
        const protocol::EncodeFrameResult encoded = protocol::encodeFrame(frame);
        if (encoded.hasError())
        {
            return false;
        }
        bytes.append(encoded.value());
    }
    return socket.write(bytes) == bytes.size();
}

class ScriptedRenderServer final
{
public:
    // 목적: client protocol test용 ephemeral localhost TCP server 시작
    // 입력: handler: 완성된 client frame별 response script
    // 출력: 연결을 받을 수 있는 scripted server
    explicit ScriptedRenderServer(FrameHandler handler) : m_handler(std::move(handler))
    {
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this]() { acceptConnection(); });
        if (!m_server.listen(QHostAddress::LocalHost, 0))
        {
            throw std::runtime_error("Unable to start scripted Render Worker server.");
        }
    }

    // 목적: client가 연결할 ephemeral port 반환
    // 입력: 없음
    // 출력: localhost server port
    [[nodiscard]] quint16 port() const noexcept
    {
        return m_server.serverPort();
    }

private:
    // 목적: pending client socket 하나를 소유하고 readyRead parser 연결
    // 입력: 없음
    // 출력: socket이 server child로 유지됨
    void acceptConnection()
    {
        QTcpSocket* const socket = m_server.nextPendingConnection();
        if (socket == nullptr)
        {
            return;
        }
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() { readAvailable(*socket); });
    }

    // 목적: client byte stream을 frame parser에 공급하고 response script 호출
    // 입력: socket: bytes와 response channel을 소유한 connection
    // 출력: 완성 frame마다 handler 호출 또는 protocol error 시 connection abort
    void readAvailable(QTcpSocket& socket)
    {
        protocol::ParseFramesOutcome outcome = m_parser.append(socket.readAll());
        if (outcome.terminalError.has_value())
        {
            socket.abort();
            return;
        }
        for (const protocol::ProtocolFrame& frame : outcome.frames)
        {
            m_handler(socket, frame);
        }
    }

    QTcpServer m_server;
    protocol::FrameParser m_parser;
    FrameHandler m_handler;
};

// 목적: client test에 공통인 valid manual endpoint 생성
// 입력: port: scripted localhost server port
// 출력: 짧은 test timeout이 설정된 endpoint
[[nodiscard]] RemoteRenderEndpoint makeEndpoint(const quint16 port)
{
    return {QStringLiteral("127.0.0.1"), port, 1000ms, 2000ms, 500ms};
}

// 목적: protocol codec validation을 통과하는 root-relative render request 생성
// 입력: 없음
// 출력: input.CR3에서 output.jpg를 만드는 request
[[nodiscard]] RemoteRenderRequest makeRequest()
{
    return {QStringLiteral("input.CR3"), QStringLiteral("output.jpg"), {}, {}};
}

// 목적: remote executor를 별도 thread에서 실행해 test server event loop block 방지
// 입력: executor/endpoint/request/cancellationToken: 한 client operation 값
// 출력: asynchronous RemoteRenderResult future
[[nodiscard]] std::future<RemoteRenderResult> executeAsync(const RemoteRenderExecutor& executor,
                                                           const RemoteRenderEndpoint& endpoint,
                                                           const RemoteRenderRequest& request,
                                                           const core::types::CancellationToken& cancellationToken)
{
    return std::async(std::launch::async, [&executor, endpoint, request, cancellationToken]() {
        return executor.execute(endpoint, request, cancellationToken);
    });
}

TEST(RemoteRenderExecutorTest, ReturnsArtifactAfterAcceptedCoalescedSuccess)
{
    ScriptedRenderServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        ASSERT_EQ(protocol::MessageType::RenderRequest, requestFrame.messageType);
        const runtime::EncodePayloadResult succeeded =
            runtime::encodeRenderSucceededPayload({QStringLiteral("output.jpg"), 42, {1, 2, 3, 4, 5}});
        ASSERT_TRUE(succeeded.hasValue());
        ASSERT_TRUE(sendFrames(socket,
                               {{protocol::MessageType::JobAccepted, requestFrame.jobId, {}},
                                {protocol::MessageType::RenderSucceeded, requestFrame.jobId, succeeded.value()}}));
    });
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;
    std::future<RemoteRenderResult> future =
        executeAsync(executor, makeEndpoint(server.port()), makeRequest(), cancellation.token());

    ASSERT_TRUE(waitForFuture(future));
    const RemoteRenderResult result = future.get();

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QStringLiteral("output.jpg"), result.value().artifact.outputPath);
    EXPECT_EQ(42U, result.value().artifact.byteSize);
    EXPECT_EQ(5U, result.value().stats.totalNanoseconds);
}

TEST(RemoteRenderExecutorTest, NotifiesAcceptedExactlyOnceBeforeTerminalSuccess)
{
    ScriptedRenderServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        const runtime::EncodePayloadResult succeeded =
            runtime::encodeRenderSucceededPayload({QStringLiteral("output.jpg"), 42, {}});
        ASSERT_TRUE(succeeded.hasValue());
        ASSERT_TRUE(sendFrames(socket,
                               {{protocol::MessageType::JobAccepted, requestFrame.jobId, {}},
                                {protocol::MessageType::RenderSucceeded, requestFrame.jobId, succeeded.value()}}));
    });
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;
    std::atomic<int> acceptedCount{0};
    std::future<RemoteRenderResult> future = std::async(std::launch::async, [&] {
        return executor.execute(makeEndpoint(server.port()),
                                makeRequest(),
                                cancellation.token(),
                                [&acceptedCount] { acceptedCount.fetch_add(1, std::memory_order_relaxed); });
    });

    ASSERT_TRUE(waitForFuture(future));
    const RemoteRenderResult result = future.get();
    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(1, acceptedCount.load(std::memory_order_relaxed));
}

TEST(RemoteRenderExecutorTest, ReturnsCompletedSuccessBeforeTrailingInvalidHeader)
{
    ScriptedRenderServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        const runtime::EncodePayloadResult succeeded =
            runtime::encodeRenderSucceededPayload({QStringLiteral("output.jpg"), 42, {1, 2, 3, 4, 5}});
        ASSERT_TRUE(succeeded.hasValue());
        const protocol::EncodeFrameResult acceptedFrame =
            protocol::encodeFrame({protocol::MessageType::JobAccepted, requestFrame.jobId, {}});
        const protocol::EncodeFrameResult succeededFrame =
            protocol::encodeFrame({protocol::MessageType::RenderSucceeded, requestFrame.jobId, succeeded.value()});
        ASSERT_TRUE(acceptedFrame.hasValue());
        ASSERT_TRUE(succeededFrame.hasValue());
        QByteArray invalidHeader = acceptedFrame.value().first(protocol::ProtocolHeaderBytes);
        invalidHeader[0] = 'X';
        const QByteArray bytes = acceptedFrame.value() + succeededFrame.value() + invalidHeader;
        ASSERT_EQ(bytes.size(), socket.write(bytes));
    });
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;
    std::future<RemoteRenderResult> future =
        executeAsync(executor, makeEndpoint(server.port()), makeRequest(), cancellation.token());

    ASSERT_TRUE(waitForFuture(future));
    const RemoteRenderResult result = future.get();

    ASSERT_TRUE(result.hasValue());
    EXPECT_EQ(QStringLiteral("output.jpg"), result.value().artifact.outputPath);
}

TEST(RemoteRenderExecutorTest, PreservesRemoteRenderFailureAndStats)
{
    ScriptedRenderServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        const runtime::EncodePayloadResult failed = runtime::encodeRenderFailedPayload(
            {{core::types::ErrorCode::DecodeFailed, QStringLiteral("decode failed")}, {1, 2, 3, 4, 5}});
        ASSERT_TRUE(failed.hasValue());
        ASSERT_TRUE(sendFrames(socket,
                               {{protocol::MessageType::JobAccepted, requestFrame.jobId, {}},
                                {protocol::MessageType::RenderFailed, requestFrame.jobId, failed.value()}}));
    });
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;
    std::future<RemoteRenderResult> future =
        executeAsync(executor, makeEndpoint(server.port()), makeRequest(), cancellation.token());

    ASSERT_TRUE(waitForFuture(future));
    const RemoteRenderResult result = future.get();

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::RenderFailed, result.error().code);
    EXPECT_EQ(core::types::ErrorCode::DecodeFailed, result.error().cause.code);
    EXPECT_EQ(QStringLiteral("decode failed"), result.error().cause.message);
    EXPECT_EQ(5U, result.error().stats.totalNanoseconds);
}

TEST(RemoteRenderExecutorTest, DistinguishesServerAndResourceBusy)
{
    ScriptedRenderServer serverBusyServer([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        const runtime::EncodePayloadResult busy = runtime::encodeServerBusyPayload({QStringLiteral("queue full")});
        ASSERT_TRUE(busy.hasValue());
        ASSERT_TRUE(sendFrames(socket, {{protocol::MessageType::ServerBusy, requestFrame.jobId, busy.value()}}));
    });
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource firstCancellation;
    std::future<RemoteRenderResult> serverBusyFuture =
        executeAsync(executor, makeEndpoint(serverBusyServer.port()), makeRequest(), firstCancellation.token());
    ASSERT_TRUE(waitForFuture(serverBusyFuture));
    const RemoteRenderResult serverBusy = serverBusyFuture.get();
    ASSERT_TRUE(serverBusy.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::ServerBusy, serverBusy.error().code);

    ScriptedRenderServer resourceBusyServer([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        const runtime::EncodePayloadResult busy =
            runtime::encodeResourceBusyPayload({QStringLiteral("INSUFFICIENT_MEMORY"), 3000ms});
        ASSERT_TRUE(busy.hasValue());
        ASSERT_TRUE(sendFrames(socket,
                               {{protocol::MessageType::JobAccepted, requestFrame.jobId, {}},
                                {protocol::MessageType::ResourceBusy, requestFrame.jobId, busy.value()}}));
    });
    const core::types::CancellationSource secondCancellation;
    std::future<RemoteRenderResult> resourceBusyFuture =
        executeAsync(executor, makeEndpoint(resourceBusyServer.port()), makeRequest(), secondCancellation.token());
    ASSERT_TRUE(waitForFuture(resourceBusyFuture));
    const RemoteRenderResult resourceBusy = resourceBusyFuture.get();
    ASSERT_TRUE(resourceBusy.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::ResourceBusy, resourceBusy.error().code);
    ASSERT_TRUE(resourceBusy.error().retryAfter.has_value());
    EXPECT_EQ(3000ms, *resourceBusy.error().retryAfter);
}

TEST(RemoteRenderExecutorTest, SendsCancelRequestAndReturnsCancelled)
{
    bool renderReceived = false;
    bool cancelReceived = false;
    ScriptedRenderServer server([&](QTcpSocket& socket, const protocol::ProtocolFrame& frame) {
        if (frame.messageType == protocol::MessageType::RenderRequest)
        {
            renderReceived = true;
            ASSERT_TRUE(sendFrames(socket, {{protocol::MessageType::JobAccepted, frame.jobId, {}}}));
            return;
        }
        ASSERT_EQ(protocol::MessageType::CancelRequest, frame.messageType);
        cancelReceived = true;
        const runtime::EncodePayloadResult cancelled =
            runtime::encodeRenderFailedPayload({{core::types::ErrorCode::Cancelled, QStringLiteral("cancelled")}, {}});
        ASSERT_TRUE(cancelled.hasValue());
        ASSERT_TRUE(sendFrames(socket, {{protocol::MessageType::RenderFailed, frame.jobId, cancelled.value()}}));
    });
    const RemoteRenderExecutor executor;
    core::types::CancellationSource cancellation;
    std::future<RemoteRenderResult> future =
        executeAsync(executor, makeEndpoint(server.port()), makeRequest(), cancellation.token());

    ASSERT_TRUE(waitUntil([&renderReceived]() { return renderReceived; }));
    cancellation.requestCancellation();
    ASSERT_TRUE(waitForFuture(future));
    const RemoteRenderResult result = future.get();

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::Cancelled, result.error().code);
    EXPECT_TRUE(cancelReceived);
}

TEST(RemoteRenderExecutorTest, ReturnsConnectionFailedForClosedEndpoint)
{
    QTcpServer probe;
    ASSERT_TRUE(probe.listen(QHostAddress::LocalHost, 0));
    const quint16 closedPort = probe.serverPort();
    probe.close();
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;

    const RemoteRenderResult result = executor.execute(makeEndpoint(closedPort), makeRequest(), cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::ConnectionFailed, result.error().code);
}

TEST(RemoteRenderExecutorTest, ReturnsTimedOutAfterAcceptedJobHasNoTerminalResponse)
{
    ScriptedRenderServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        ASSERT_TRUE(sendFrames(socket, {{protocol::MessageType::JobAccepted, requestFrame.jobId, {}}}));
    });
    RemoteRenderEndpoint endpoint = makeEndpoint(server.port());
    endpoint.renderTimeout = 100ms;
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;
    std::future<RemoteRenderResult> future = executeAsync(executor, endpoint, makeRequest(), cancellation.token());

    ASSERT_TRUE(waitForFuture(future));
    const RemoteRenderResult result = future.get();

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::TimedOut, result.error().code);
}

TEST(RemoteRenderExecutorTest, RejectsMismatchedResponseJobId)
{
    ScriptedRenderServer server([](QTcpSocket& socket, const protocol::ProtocolFrame& requestFrame) {
        ASSERT_TRUE(sendFrames(
            socket, {{protocol::MessageType::JobAccepted, static_cast<protocol::JobId>(requestFrame.jobId + 1), {}}}));
    });
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;
    std::future<RemoteRenderResult> future =
        executeAsync(executor, makeEndpoint(server.port()), makeRequest(), cancellation.token());

    ASSERT_TRUE(waitForFuture(future));
    const RemoteRenderResult result = future.get();

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::ProtocolViolation, result.error().code);
}

TEST(RemoteRenderExecutorTest, RejectsInvalidEndpointBeforeConnecting)
{
    const RemoteRenderExecutor executor;
    const core::types::CancellationSource cancellation;
    RemoteRenderEndpoint endpoint;

    const RemoteRenderResult result = executor.execute(endpoint, makeRequest(), cancellation.token());

    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(RemoteRenderErrorCode::InvalidEndpoint, result.error().code);
    EXPECT_EQ(core::types::ErrorCode::InvalidArgument, result.error().cause.code);
}

}  // namespace
}  // namespace flexraw::worker::client
